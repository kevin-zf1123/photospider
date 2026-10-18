#include "execution/structured_execution.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/callback_environment.hpp"
#include "core/resource_observation.hpp"
#include "execution/result_blocks.hpp"
#include "execution/result_gpu_service.hpp"
#include "execution/result_native.hpp"
#include "execution/result_protocol.hpp"
#include "execution/result_publication_validation.hpp"
#include "execution/result_subscription.hpp"
#include "execution/structured_cache_state.hpp"
#include "execution/tile_records.hpp"
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif
#include "photospider/core/content_digest.hpp"
#include "plugin/dependency_identity.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/publication_visibility.hpp"
#include "plugin/result_payload_bound.hpp"
#include "plugin/validation_dispatch.hpp"

namespace ps::execution_internal {
/** @brief Interpreter for compiler-expanded tile instructions and CFG edges.
 * It never constructs producer tasks, wait edges, tile mappings or schedules.
 * Each instruction binds fixed ready values, invokes its operation and stores
 * the certified publication. Block transitions copy arguments in parallel.
 */
class StructuredExecution final {
 public:
  StructuredExecution(const ExecutionPlan& plan,
                      std::vector<ExecutionBinding> bindings,
                      std::shared_ptr<OperationRegistry> operations,
                      ResourceBudget resources, const ExecutionOptions& options,
                      CancellationToken cancellation,
                      std::function<ErrorCode()> stop,
                      StructuredDispatch dispatch, std::string snapshot,
                      std::shared_ptr<const ExecutionPlan> owner,
                      ResultCheckpoints* checkpoints, ResultCache* cache)
      : owner_(std::move(owner)),
        plan_(owner_ ? *owner_ : plan),
        bindings_(std::move(bindings)),
        operations_(std::move(operations)),
        resources_(std::move(resources)),
        options_(options),
        cancellation_(std::move(cancellation)),
        stop_(std::move(stop)),
        dispatch_(std::move(dispatch)),
        snapshot_(std::move(snapshot)),
        subscription_(resources_, cancellation_, options_.result_publication),
        remaining_(options_.maximum_dependency_work),
        control_(resources_),
        cache_(cache),
        checkpoints_(checkpoints),
        cache_epoch_(cache ? cache->epoch() : 0),
        cache_state_(resources_, options_.maximum_dependency_cache_work) {
    static std::atomic<std::uint64_t> next{1};
    run_identity_ = next.load();
    do {
      if (run_identity_ == UINT64_MAX)
        throw std::bad_alloc();
    } while (!next.compare_exchange_weak(run_identity_, run_identity_ + 1));
    diagnostics_.operation_timings = decltype(diagnostics_.operation_timings)(
        ResourceAllocator<OperationTiming>(resources_));
    diagnostics_.plan_digest = ResourceString(
        plan_.digest().value, ResourceAllocator<char>(resources_));
    subscription_.initialize(plan_.steps().size());
  }
  Result<ExecutionResult> run() {
    const auto started = std::chrono::steady_clock::now();
    ResourceAllocationScope allocation(resources_);
    auto selected = plan_.resources().reference(resources_);
    if (!selected.ok())
      return Result<ExecutionResult>(selected.status());
    selected_resources_ = selected.take_value();
    auto capacity = resources_.reserve(ResourceCapacity::host(
        plan_.steps().size() * (sizeof(ValueState) + sizeof(ResultRef)) +
            plan_.parameters().size() * sizeof(ResultRef),
        plan_.steps().size() * (sizeof(ValueState) + sizeof(ResultRef)) +
            plan_.parameters().size() * sizeof(ResultRef)));
    if (!capacity.ok())
      return Result<ExecutionResult>(capacity.status());
    state_capacity_ = capacity.take_value();
    values_.reserve(plan_.steps().size());
    for (std::size_t i = 0; i < plan_.steps().size(); ++i)
      values_.emplace_back(resources_);
    records_ = std::allocate_shared<TileRecords>(
        ResourceAllocator<TileRecords>(resources_), resources_);
    argument_records_.resize(plan_.parameters().size());
    arguments_.resize(plan_.parameters().size());
    externals_.resize(plan_.input_declarations().size());
    for (std::size_t i = 0; i < externals_.size(); ++i) {
      const auto& declaration = plan_.input_declarations()[i];
      auto found = std::find_if(bindings_.begin(), bindings_.end(),
                                [&](const auto& binding) {
                                  return binding.name == declaration.name;
                                });
      if (found == bindings_.end())
        return Result<ExecutionResult>(protocol_failure("missing bound input"));
      externals_[i] = found->result;
    }
    std::size_t pc = plan_.entry();
    std::vector<ResultRef> incoming;
    std::vector<std::shared_ptr<const TileRecord>> incoming_records;
    for (;;) {
      auto status =
          consume(1);  // Empty loops still observe cancellation/budgets.
      if (!status.ok())
        return Result<ExecutionResult>(status);
      if (pc >= plan_.blocks().size())
        return Result<ExecutionResult>(
            protocol_failure("invalid plan program counter"));
      const auto& block = plan_.blocks()[pc];
      if (incoming.size() != block.parameters.size())
        return Result<ExecutionResult>(
            protocol_failure("plan edge arity mismatch"));
      if (activation_ == UINT64_MAX)
        return Result<ExecutionResult>(
            {ErrorCode::ResourceExhausted, "activation overflow"});
      ++activation_;
      shared_.clear();
      for (auto step : block.calls)
        values_[step] = ValueState(resources_);
      for (std::size_t i = 0; i < incoming.size(); ++i) {
        arguments_[block.parameters[i]] = std::move(incoming[i]);
        argument_records_[block.parameters[i]] = std::move(incoming_records[i]);
      }
      incoming.clear();
      incoming_records.clear();
      std::size_t instruction_index = 0, call_index = 0;
      while (call_index < block.calls.size()) {
        const auto call = plan_.steps()[block.calls[call_index]].node_id;
        // Plan validation groups all outputs and shared stages by call. Run
        // the entire group before certifying any of its assembled outputs.
        while (instruction_index < block.instructions.size() &&
               plan_.steps()[block.instructions[instruction_index].step_index]
                       .node_id == call) {
          const auto& instruction = block.instructions[instruction_index++];
          status = execute(instruction);
          if (!status.ok())
            return Result<ExecutionResult>(
                call_failure(std::move(status), instruction.step_index));
        }
        // Reject an invalid upstream result before starting the next call,
        // including consumers that only read descriptors and effectful calls.
        do {
          const auto step = block.calls[call_index++];
          if (!values_[step].tiles.empty()) {
            auto certified = value(PlanStepInput{step});
            if (!certified.ok())
              return Result<ExecutionResult>(
                  call_failure(certified.status(), step));
          }
        } while (call_index < block.calls.size() &&
                 plan_.steps()[block.calls[call_index]].node_id == call);
      }
      const PlanEdge* next = nullptr;
      if (const auto* jump = std::get_if<PlanJump>(&block.terminator)) {
        next = &jump->edge;
      } else if (const auto* branch =
                     std::get_if<PlanConditionalJump>(&block.terminator)) {
        auto condition = value(branch->condition);
        if (!condition.ok())
          return Result<ExecutionResult>(condition.status());
        auto facts = condition.value().descriptor();
        if (!facts.ok())
          return Result<ExecutionResult>(facts.status());
        std::uint8_t boolean = 0;
        const auto shape = condition.value().schema().tensors[0].sample_shape();
        status = condition.value().read_tensor(
            facts.value(), 0, std::vector<std::uint64_t>(shape.size(), 0),
            &boolean, sizeof(boolean), cancellation_);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        status = input_record(branch->condition, nullptr, control_);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        control_cacheable_ &= reusable_value(branch->condition);
        control_pure_ &= pure_value(branch->condition);
        next = boolean ? &branch->if_true : &branch->if_false;
      } else {
        ExecutionResult result;
        result.results = make_resource_map<ResultRef>(resources_);
        const auto& returned = std::get<PlanReturn>(block.terminator);
        for (const auto& named : plan_.outputs()) {
          const auto& output = plan_.output(named.first);
          ResultRef source;
          if (!plan_.output_requests().at(named.first).empty()) {
            auto captured = value(returned.values.at(named.second));
            if (!captured.ok())
              return Result<ExecutionResult>(captured.status());
            source = captured.take_value();
          }
          auto projected =
              project(*output.result_schema, source, output.requested_samples,
                      "return/" + named.first);
          if (!projected.ok())
            return Result<ExecutionResult>(projected.status());
          status = record_returns(output, returned.values.at(named.second),
                                  plan_.output_requests().at(named.first),
                                  projected.value());
          result.results.emplace(
              ResourceString(named.first, ResourceAllocator<char>(resources_)),
              projected.take_value());
          if (!status.ok())
            return Result<ExecutionResult>(status);
          diagnostics_.tile_count +=
              plan_.output_requests().at(named.first).size();
        }
        status = consume(0);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        status = digest(result);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        subscription_.close();
        if (!subscription_.status().ok())
          return Result<ExecutionResult>(subscription_.status());
        diagnostics_.execute_us = micros(started);
        diagnostics_.managed_resources = resources_.statistics();
        status = records_->retire_payloads();
        if (status.ok())
          status = consume(0);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        result.dependencies.tiles_ = records_;
        result.diagnostics = std::move(diagnostics_);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_final_result_ready();
#endif
        status = consume(0);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        return Result<ExecutionResult>(std::move(result));
      }
      // Capture every source owner before replacing any target slot.
      for (const auto& argument : next->arguments) {
        auto captured = value(argument);
        if (!captured.ok())
          return Result<ExecutionResult>(captured.status());
        incoming.push_back(captured.take_value());
        TileRecord evidence(resources_);
        status = input_record(argument, nullptr, evidence);
        if (status.ok())
          status = merge_record(evidence, control_);
        if (!status.ok())
          return Result<ExecutionResult>(status);
        auto frozen = capture_record(evidence);
        if (!frozen.ok())
          return Result<ExecutionResult>(frozen.status());
        incoming_records.push_back(frozen.take_value());
      }
      pc = next->target;
    }
  }

 private:
  Status call_failure(Status status, std::size_t step) const {
    if (!status.detail.node_id && !status.detail.input_id)
      status.detail.node_id = plan_.steps()[step].node_id;
    if (status.detail.scope == FailureScope::Unspecified)
      status.detail.scope = FailureScope::Group;
    return status;
  }
  struct StoredTile {
    ResultRef result;
    const std::vector<Footprint>* samples = nullptr;
    std::shared_ptr<const TileRecord> evidence;
    std::string cache_key;
  };
  struct ValueState {
    explicit ValueState(const ResourceBudget& root)
        : tiles(ResourceAllocator<StoredTile>(root)) {}
    ResourceVector<StoredTile> tiles;
    ResultRef assembled;
    bool cacheable = true;
    bool pure = true;
    bool individually_certified = false;
    std::shared_ptr<plugin_internal::ResultPayloadBound> payload_bound;
  };
  struct UnitState {
    UnitState(const PlanStep& step, const ResourceBudget& resources)
        : contract(step),
          query(*step.structured_metadata, step.parameters),
          tensors(std::less<std::pair<std::uint32_t, std::uint32_t>>{},
                  ResourceAllocator<ResultTensorInputs::value_type>(resources)),
          descriptors(
              std::less<std::uint32_t>{},
              ResourceAllocator<ResultDescriptorInputs::value_type>(resources)),
          objects(std::less<std::uint32_t>{},
                  ResourceAllocator<ResultObjectInputs::value_type>(resources)),
          shared(std::less<std::uint32_t>{},
                 ResourceAllocator<ResultObjectInputs::value_type>(resources)),
          failure(std::allocate_shared<std::atomic<ErrorCode>>(
              ResourceAllocator<std::atomic<ErrorCode>>(resources),
              ErrorCode::Ok)),
          latch(std::allocate_shared<plugin_internal::FailureLatch>(
              ResourceAllocator<plugin_internal::FailureLatch>(resources))),
          record(resources) {}
    const PlanStep& contract;
    ResultProgramQuery query;
    std::string key, content_key, block_content_key;
    bool pure = false, cacheable = false, proof_ready = false, reused = false,
         fallback_tainted = false;
    bool block_proof_ready = false;
    ResultTensorInputs tensors;
    ResultDescriptorInputs descriptors;
    ResultObjectInputs objects, shared;
    std::shared_ptr<std::atomic<ErrorCode>> failure;
    std::shared_ptr<plugin_internal::FailureLatch> latch;
    ResultContinuation continuation;
    ResultRef published, raw_publication;
    std::optional<ResultBuilder> visible_builder;
    TileRequest visible_tiles;
    std::shared_ptr<plugin_internal::ResultPayloadBound> payload_bound;
    TileRecord record;
    std::uint64_t revision = 0, polls = 0;
    std::atomic<std::uint64_t> remaining{UINT64_MAX};
    std::atomic<std::uint64_t> discovery_remaining{0};
    std::atomic<bool> in_discovery{false};
    bool started = false, complete = false;
  };
  static std::uint64_t micros(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }
  ErrorCode stopped() const {
    if (cancellation_.cancelled())
      return ErrorCode::Cancelled;
    if (!plan_.current())
      return ErrorCode::Stale;
    if (subscription_.code() != ErrorCode::Ok)
      return subscription_.code();
    return stop_ ? stop_() : ErrorCode::Ok;
  }
  Status consume(std::uint64_t count) {
    {
      std::lock_guard<std::recursive_mutex> lock(cache_mutex_);
      if (!cache_failure_.ok())
        return cache_failure_;
    }
    auto stop = stopped();
    if (stop != ErrorCode::Ok)
      return {stop, {}};
    auto available = remaining_.load(std::memory_order_relaxed);
    do {
      if (count > available)
        return {ErrorCode::ResourceExhausted,
                "static execution metadata work limit",
                FailureReason::WorkLimit,
                {FailureOrigin::Resource, FailureScope::Run}};
    } while (!remaining_.compare_exchange_weak(available, available - count,
                                               std::memory_order_relaxed));
    return resources_.consume({count});
  }
  Status work(UnitState& unit, std::uint64_t count) {
    Status status{stopped(), {}};
    if (status.ok() && unit.in_discovery.load()) {
      for (auto* remaining : {&unit.discovery_remaining, &remaining_}) {
        auto available = remaining->load(std::memory_order_relaxed);
        do {
          if (count > available) {
            status = {ErrorCode::ResourceExhausted,
                      "static GPU selection work limit",
                      FailureReason::WorkLimit,
                      {FailureOrigin::Resource, FailureScope::Run}};
            break;
          }
        } while (!remaining->compare_exchange_weak(available, available - count,
                                                   std::memory_order_relaxed));
        if (!status.ok())
          break;
      }
    }
    if (status.ok() && unit.query.shared_stage != UINT32_MAX) {
      auto available = unit.remaining.load(std::memory_order_relaxed);
      do {
        if (count > available) {
          status = {ErrorCode::ResourceExhausted, "shared stage work limit",
                    FailureReason::WorkLimit};
          break;
        }
      } while (!unit.remaining.compare_exchange_weak(
          available, available - count, std::memory_order_relaxed));
    }
    if (status.ok())
      status = resources_.consume({count});
    if (!status.ok()) {
      unit.latch->record(status);
      auto expected = ErrorCode::Ok;
      unit.failure->compare_exchange_strong(expected, status.code);
    }
    return status;
  }
  std::string key(const std::string& suffix) const {
    content_internal::Sha256 digest;
    digest.text("photospider.static-invocation.v1");
    digest.text(plan_.digest().value);
    digest.text(snapshot_);
    digest.integer(run_identity_);
    digest.integer(activation_);
    digest.text(suffix);
    return digest.finish();
  }
  Status dispatch(
      Backend backend, bool tiles,
      const std::function<Status(const StructuredServices&)>& compute) {
    auto status = consume(1);
    if (!status.ok())
      return status;
    auto observations = std::allocate_shared<ExecutionDiagnostics>(
        ResourceAllocator<ExecutionDiagnostics>(resources_));
    auto payload = core_internal::ResourcePayloadScope::capture(resources_);
    auto submitted = dispatch_(
        backend, tiles, cancellation_,
        [&, observations, payload](const StructuredServices& services) {
          core_internal::ResourcePayloadScope payload_scope(resources_,
                                                            payload);
          ResultNativeScope native(services.allocator, services.native_input,
                                   resources_, options_.dependencies.sets,
                                   options_.dependencies.maximum_gpu_requests,
                                   services.native_storage,
                                   services.gpu == nullptr,
                                   [&](auto n) { return consume(n); });
          auto result = compute(services);
          observations->transfer_count += native.transfers;
          observations->transfer_bytes += native.transferred_bytes;
          observations->host_access_count += native.host_accesses;
          if (services.observe)
            services.observe(*observations);
          return result;
        });
    if (!submitted.ok())
      return submitted.status();
    auto pending = submitted.take_value();
    struct Drain {
      const StructuredSubmission& submission;
      ~Drain() { submission.wait(); }
    } drain{pending};
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    // The static coordinator owns this submission until its queue owner
    // retires, even when cancellation arrives before a worker picks it up.
    execution_testing::notify_post_submit_observation();
#endif
    pending.wait();
    status = pending.finish();
    diagnostics_.peak_active_tasks = 1;
    diagnostics_.transfer_count += observations->transfer_count;
    diagnostics_.transfer_bytes += observations->transfer_bytes;
    diagnostics_.host_access_count += observations->host_access_count;
    diagnostics_.cpu_stage_count += observations->cpu_stage_count;
    diagnostics_.cpu_tile_callback_count +=
        observations->cpu_tile_callback_count;
    diagnostics_.native_dispatch_count += observations->native_dispatch_count;
    diagnostics_.native_submission_count +=
        observations->native_submission_count;
    diagnostics_.native_compute_us += observations->native_compute_us;
    diagnostics_.native_constant_bytes += observations->native_constant_bytes;
    diagnostics_.native_upload_hits += observations->native_upload_hits;
    return status;
  }
  Result<ResultRef> project(const SchemaTemplate& schema,
                            const ResultRef& source,
                            const std::vector<Footprint>& samples,
                            const std::string& suffix) {
    auto made = ResultBuilder::start(resources_, schema, key(suffix), {},
                                     plan_.tile_extent());
    if (!made.ok())
      return Result<ResultRef>(made.status());
    auto builder = made.take_value();
    bool any = false;
    std::optional<ResultDescriptor> facts;
    if (source.valid()) {
      auto described = source.descriptor(false);
      if (!described.ok())
        return Result<ResultRef>(described.status());
      facts = described.take_value();
    }
    for (std::uint32_t member = 0; member < samples.size(); ++member) {
      if (!samples[member].empty() && !facts)
        return Result<ResultRef>(
            protocol_failure("planned source is not ready"));
      for (const auto& region : samples[member].boxes()) {
        auto status = plugin_internal::append_publication_view(
            builder, source, *facts, member, region, resources_, cancellation_,
            [&](auto n) { return consume(n); });
        if (!status.ok())
          return Result<ResultRef>(status);
        any = true;
      }
    }
    return any ? builder.seal() : builder.seal_empty();
  }
  Result<ResultRef> value(const PlanInput& input) {
    if (const auto* declaration = std::get_if<PlanWorkflowInput>(&input))
      return Result<ResultRef>(externals_.at(declaration->declaration_index));
    if (const auto* parameter = std::get_if<PlanArgumentInput>(&input)) {
      const auto& result = arguments_.at(parameter->parameter_index);
      return result.valid() ? Result<ResultRef>(result)
                            : Result<ResultRef>(protocol_failure(
                                  "block parameter is not ready"));
    }
    auto index = std::get<PlanStepInput>(input).step_index;
    auto& saved = values_.at(index);
    if (saved.assembled.valid())
      return Result<ResultRef>(saved.assembled);
    if (saved.tiles.empty())
      return Result<ResultRef>(
          protocol_failure("planned producer is not ready"));
    const auto& step = plan_.steps()[index];
    auto made = ResultBuilder::start(resources_, *step.output_result_schema,
                                     key("value/" + std::to_string(index)), {},
                                     plan_.tile_extent());
    if (!made.ok())
      return Result<ResultRef>(made.status());
    auto builder = made.take_value();
    for (const auto& tile : saved.tiles) {
      auto facts = tile.result.descriptor();
      if (!facts.ok())
        return Result<ResultRef>(facts.status());
      for (std::uint32_t member = 0; member < tile.samples->size(); ++member)
        for (const auto& region : (*tile.samples)[member].boxes()) {
          auto status = plugin_internal::append_publication_view(
              builder, tile.result, facts.value(), member, region, resources_,
              cancellation_, [&](auto n) { return consume(n); });
          if (!status.ok())
            return Result<ResultRef>(status);
        }
    }
    auto sealed = builder.seal();
    if (!sealed.ok())
      return sealed;
    saved.assembled = sealed.take_value();
    saved.assembled.bind_producer(step.node_id);
    auto facts = saved.assembled.descriptor();
    if (!facts.ok())
      return Result<ResultRef>(facts.status());
    // Complete validators observe the actual compiled aggregate, never infer
    // additional producer work from an incomplete publication.
    auto status = Status::success();
    if (!saved.individually_certified)
      status = plugin_internal::validate_result_contract(
          *step.prepared, true, step.output_index, saved.assembled,
          facts.value(), nullptr, resources_, cancellation_,
          [&](auto n) { return consume(n); },
          options_.maximum_result_window_bytes, ErrorCode::OperationFailed);
    if (!status.ok())
      return Result<ResultRef>(status);
    for (auto& tile : saved.tiles) {
      status = retain(tile, step.backend);
      if (!status.ok())
        return Result<ResultRef>(status);
    }
    if (!saved.individually_certified)
      status = subscription_.notify(index, step.result_ref(), saved.assembled,
                                    facts.value().revision());
    return status.ok() ? Result<ResultRef>(saved.assembled)
                       : Result<ResultRef>(status);
  }
  Status merge_record(TileRecord& target, const TileRecord& source) {
    for (const auto& dependency : source.inputs) {
      auto status = consume(1);
      if (status.ok())
        status = target.include(dependency, resources_);
      if (!status.ok())
        return status;
    }
    return Status::success();
  }
  Result<std::shared_ptr<const TileRecord>> capture_record(
      const TileRecord& record) {
    auto copy = std::allocate_shared<TileRecord>(
        ResourceAllocator<TileRecord>(resources_), resources_);
    copy->unit = record.unit;
    auto status = merge_record(*copy, record);
    if (!status.ok())
      return Result<std::shared_ptr<const TileRecord>>(status);
    return Result<std::shared_ptr<const TileRecord>>(std::move(copy));
  }
  Status input_record(const PlanInput& input, const TileRequest* selected,
                      TileRecord& record, bool descriptor_only = false) {
    if (const auto* declaration = std::get_if<PlanWorkflowInput>(&input)) {
      if (descriptor_only)
        return record.include({input, 0, 0, {}}, resources_);
      const auto& grids = plan_.input_grids(input);
      // This reads the compiled tile interval program; it does not discover
      // producers or map coordinates. Full edge arguments are already ready.
      if (selected) {
        for (const auto& member : selected->members) {
          auto status = record.include({input, 0, member.first, member.second},
                                       resources_);
          if (!status.ok())
            return status;
        }
      } else {
        for (std::uint32_t slot = 0; slot < grids.size(); ++slot) {
          auto all = TileSet::range(resources_, 0, grids[slot].tile_count());
          if (!all.ok())
            return all.status();
          auto status =
              record.include({PlanWorkflowInput{declaration->declaration_index},
                              0, slot, all.take_value()},
                             resources_);
          if (!status.ok())
            return status;
        }
      }
      return Status::success();
    }
    if (const auto* parameter = std::get_if<PlanArgumentInput>(&input)) {
      const auto& evidence = argument_records_.at(parameter->parameter_index);
      return evidence
                 ? merge_record(record, *evidence)
                 : protocol_failure("block parameter evidence is not ready");
    }
    // A call's descriptor is immutable IR metadata and needs no payload task.
    if (descriptor_only)
      return Status::success();
    const auto& saved = values_.at(std::get<PlanStepInput>(input).step_index);
    for (const auto& tile : saved.tiles) {
      if (selected) {
        auto member = selected->members.find(tile.evidence->unit.tensor_slot);
        if (member == selected->members.end() ||
            !member->second.contains(tile.evidence->unit.tile))
          continue;
      }
      auto status = merge_record(record, *tile.evidence);
      if (!status.ok())
        return status;
    }
    return Status::success();
  }
  Status record_returns(const PlanOutput& output, const PlanInput& input,
                        const TileRequest& selected, const ResultRef& payload) {
    for (const auto& member : selected.members)
      for (auto tile : member.second) {
        TileRecord record(resources_);
        record.unit = {output.record_index, 0, member.first, tile};
        auto one = TileSet::range(resources_, tile, tile + 1);
        if (!one.ok())
          return one.status();
        TileRequest request;
        request.members.emplace(member.first, one.take_value());
        auto status = input_record(input, &request, record);
        if (status.ok())
          status = merge_record(record, control_);
        if (status.ok())
          status = records_->install(0, record, payload, nullptr, nullptr,
                                     [&](auto n) { return consume(n); });
        if (!status.ok())
          return status;
      }
    return Status::success();
  }
  Status bind(UnitState& unit, const PlanTileInstruction& instruction) {
    unit.pure = !unit.fallback_tainted && control_pure_ &&
                unit.contract.traits.deterministic &&
                unit.contract.traits.side_effect_free &&
                instruction.shared_stage == UINT32_MAX &&
                instruction.shared_inputs.empty();
    unit.cacheable =
        unit.pure && control_cacheable_ && unit.contract.traits.cacheable;
    for (const auto& read : instruction.inputs)
      if (!read.descriptor_only) {
        unit.pure &= pure_value(unit.contract.inputs[read.input]);
        unit.cacheable &= reusable_value(unit.contract.inputs[read.input]);
      }
    unit.cacheable &= unit.pure;
    auto inherited = merge_record(unit.record, control_);
    if (!inherited.ok())
      return inherited;
    unit.tensors.clear();
    unit.descriptors.clear();
    unit.shared.clear();
    for (const auto& read : instruction.inputs) {
      ResultTensorInput input;
      input.slot_ = read.tensor_slot;
      input.samples_ = read.samples;
      input.resources_ = resources_;
      input.failure_ = unit.failure;
      input.observer_ = unit.latch;
      if (read.descriptor_only) {
        const auto& schema = unit.query.inputs.at(read.input).result_schema;
        ResultDescriptor descriptor;
        descriptor.tensor_count_ =
            static_cast<std::uint32_t>(schema->tensors.size());
        for (std::uint32_t member = 0; member < schema->tensors.size();
             ++member) {
          auto empty = Footprint::none(schema->tensors[member].sample_shape());
          if (!empty.ok())
            return empty.status();
          descriptor.tensors_[member] = empty.take_value();
        }
        unit.descriptors.insert_or_assign(read.input, std::move(descriptor));
      } else {
        if (read.tiles.empty())
          continue;
        auto source = value(unit.contract.inputs.at(read.input));
        if (!source.ok())
          return source.status();
        auto facts = source.value().descriptor();
        if (!facts.ok())
          return facts.status();
        if (!source.value().schema().same_schema(
                *unit.query.inputs[read.input].result_schema))
          return protocol_failure("planned input schema mismatch");
        auto outside = read.samples.subtract(
            facts.value().tensor_coverage(read.tensor_slot),
            options_.dependencies.sets);
        if (!outside.ok())
          return outside.status();
        if (!outside.value().empty())
          return protocol_failure("planned input coverage is not ready");
        input.result_ = source.take_value();
        input.descriptor_ = facts.take_value();
        input.payload_authorized_ = true;
        unit.descriptors.try_emplace(read.input, input.descriptor_);
      }
      const auto address = std::make_pair(read.input, read.tensor_slot);
      if (!read.descriptor_only)
        unit.tensors.insert_or_assign(address, std::move(input));
      TileRequest selected;
      if (!read.tiles.empty())
        selected.members.emplace(read.tensor_slot, read.tiles);
      auto recorded = input_record(unit.contract.inputs[read.input], &selected,
                                   unit.record, read.descriptor_only);
      if (!recorded.ok())
        return recorded;
    }
    for (const auto& read : instruction.shared_inputs) {
      const auto found = shared_.find({unit.contract.node_id, read.stage});
      if (read.tiles.empty())
        continue;
      if (found == shared_.end() || !found->second->published.valid())
        return protocol_failure("planned shared publication is not ready");
      auto captured = project(*found->second->contract.output_result_schema,
                              found->second->published, read.samples,
                              "shared-read/" + std::to_string(read.stage));
      if (!captured.ok())
        return captured.status();
      unit.shared.emplace(read.stage, captured.take_value());
      auto recorded = merge_record(unit.record, found->second->record);
      if (!recorded.ok())
        return recorded;
    }
    for (std::uint32_t port = 0; port < unit.contract.inputs.size(); ++port) {
      const auto& input = unit.contract.inputs[port];
      const auto* external = std::get_if<PlanWorkflowInput>(&input);
      auto status = plugin_internal::validate_tensor_contracts(
          *unit.query.prepared, port, *unit.query.inputs[port].result_schema,
          unit.tensors, resources_, cancellation_,
          [&](auto n) { return work(unit, n); },
          external ? ErrorCode::InvalidArgument : ErrorCode::OperationFailed);
      if (!status.ok()) {
        if (!status.detail.node_id && !status.detail.input_id) {
          if (external)
            status.detail.input_id =
                plan_.input_declarations()[external->declaration_index].id;
          else if (const auto* producer = std::get_if<PlanStepInput>(&input))
            status.detail.node_id = plan_.steps()[producer->step_index].node_id;
        }
        return status;
      }
    }
    return Status::success();
  }
  bool reusable_value(const PlanInput& input) const {
    if (const auto* source = std::get_if<PlanStepInput>(&input))
      return values_[source->step_index].cacheable;
    // Parameter values are mutable across activations. Until a parameter's
    // purity provenance is proved, optional caches conservatively skip it.
    return std::holds_alternative<PlanWorkflowInput>(input);
  }
  bool pure_value(const PlanInput& input) const {
    if (const auto* source = std::get_if<PlanStepInput>(&input))
      return values_[source->step_index].pure;
    return std::holds_alternative<PlanWorkflowInput>(input);
  }
  Status cache_work(std::uint64_t amount) {
    auto status = Status{stopped(), {}};
    if (!status.ok()) {
      cache_failure_ = status;
      return status;
    }
    if (amount > cache_state_.remaining()) {
      cache_state_.disable();
      return {ErrorCode::ResourceExhausted, "optional static cache quota"};
    }
    status = cache_state_.charge(amount);
    if (!status.ok())
      cache_failure_ = status;
    else
      diagnostics_.dependency_cache_work += amount;
    return status;
  }
  std::string block_contract(UnitState& unit) {
    auto available = cache_state_.remaining();
    contract_internal::DependencyTemplateDigest hash(
        &available, [&](auto n) { return cache_work(n).ok(); });
    hash.text("photospider.static-block-contract.v1");
    hash.text(unit.contract.operation);
    hash.integer(static_cast<std::uint32_t>(plan_.execution_mode()));
    hash.integer(static_cast<std::uint32_t>(unit.query.backend));
    hash.integer(unit.query.tile_extent);
    const bool shared = unit.contract.traits.share_blocks_across_outputs;
    if (!shared)
      hash.integer(unit.query.output_index);
    contract_internal::append_traits(
        &hash, shared ? unit.query.prepared->traits() : unit.contract.traits);
    hash.integer(unit.query.prepared->resource_ids().size());
    for (const auto& id : unit.query.prepared->resource_ids()) {
      hash.integer(id.byte_length);
      for (auto byte : id.sha256)
        hash.integer(byte);
    }
    hash.integer(unit.query.parameters.size());
    for (const auto& parameter : unit.query.parameters) {
      hash.text(parameter.first);
      hash.integer(parameter.second.index());
      if (const auto* number = std::get_if<std::int64_t>(&parameter.second)) {
        hash.integer(*number);
      } else if (const auto* real = std::get_if<double>(&parameter.second)) {
        std::uint64_t bits;
        std::memcpy(&bits, real, sizeof(bits));
        hash.integer(bits);
      } else if (const auto* boolean = std::get_if<bool>(&parameter.second)) {
        hash.integer(*boolean);
      } else {
        hash.text(std::get<std::string>(parameter.second));
      }
    }
    hash.integer(unit.query.inputs.size());
    for (const auto& input : unit.query.inputs)
      hash.schema(*input.result_schema);
    return hash.finish();
  }
  Status cache_proof(UnitState& unit, const PlanTileInstruction& instruction,
                     bool block = false) {
    std::lock_guard<std::recursive_mutex> lock(cache_mutex_);
    auto& ready = block ? unit.block_proof_ready : unit.proof_ready;
    if (ready)
      return cache_failure_;
    ready = true;
    if (!unit.pure || !cache_state_.remaining() ||
        instruction.step_index >= plan_.structured_templates().size())
      return Status::success();
    ResourceAllocationScope optional(resources_);
    try {
      const auto contract =
          block ? block_contract(unit)
                : plan_.structured_templates()[instruction.step_index];
      if (contract.empty())
        return cache_failure_;
      auto charged = cache_work(contract.size() + 16);
      if (!charged.ok())
        return cache_failure_;
      content_internal::Sha256 hash;
      hash.text(block ? "photospider.fixed-block-inputs.v2"
                      : "photospider.fixed-grant-cache.v2");
      hash.text(contract);
      hash.integer(static_cast<std::uint32_t>(unit.query.backend));
      if (!block) {
        hash.integer(instruction.tensor_slot);
        hash.integer(instruction.tile_unit);
        for (const auto& samples : instruction.publication_samples)
          append_block_footprint(&hash, samples);
      }
      charged = cache_work(instruction.inputs.size() * 2 + 1);
      if (!charged.ok())
        return cache_failure_;
      hash.integer(
          std::count_if(instruction.inputs.begin(), instruction.inputs.end(),
                        [](const auto& read) { return read.descriptor_only; }));
      for (const auto& read : instruction.inputs)
        if (read.descriptor_only)
          hash.integer(read.input);
      hash.integer(unit.tensors.size());
      for (const auto& item : unit.tensors) {
        const auto& input = item.second;
        hash.integer(item.first.first);
        hash.integer(item.first.second);
        const auto& schema = *unit.query.inputs[item.first.first].result_schema;
        charged = cache_work(schema.canonical_size() + 4);
        if (!charged.ok())
          return cache_failure_;
        hash.text(schema.canonical());
        hash.integer(input.payload_authorized_);
        if (!input.payload_authorized_)
          continue;
        charged = append_block_samples(
            &hash, input.samples_, input.spec().descriptor.element_type,
            [&](const auto& at, void* data, std::size_t bytes) {
              auto status = cache_work(1);
              return status.ok()
                         ? input.read_granted(at, data, bytes, cancellation_)
                         : status;
            },
            [&](auto n) { return cache_work(n); }, cancellation_);
        if (!charged.ok())
          return cache_failure_.ok() &&
                         charged.code == ErrorCode::ResourceExhausted
                     ? Status::success()
                     : charged;
      }
      (block ? unit.block_content_key : unit.content_key) = hash.finish();
      ++diagnostics_.dependency_cache_records_visited;
      return Status::success();
    } catch (const std::bad_alloc&) {
      return Status::success();
    }
  }
  Status cache_allowed(const UnitState& unit) {
    auto status = consume(0);
    if (!status.ok())
      return status;
    if (!unit.latch->snapshot().ok())
      return unit.latch->snapshot();
    if (unit.query.shared_stage != UINT32_MAX ||
        !unit.contract.traits.deterministic ||
        !unit.contract.traits.side_effect_free)
      return protocol_failure(
          "checkpoint and block services require a pure ordinary unit");
    return Status::success();
  }
  Result<std::optional<ResultCheckpoint>> checkpoint_before(
      UnitState& unit, const PlanTileInstruction& instruction,
      std::uint32_t phase, std::uint64_t before) {
    using Answer = Result<std::optional<ResultCheckpoint>>;
    std::unique_lock<std::recursive_mutex> lock(cache_mutex_);
    auto status = cache_allowed(unit);
    if (!status.ok())
      return Answer(status);
    if (!phase)
      return Answer(protocol_failure("zero checkpoint phase"));
    status = cache_proof(unit, instruction);
    if (!status.ok())
      return Answer(status);
    if (unit.content_key.empty() || !cache_state_.remaining())
      return Answer(std::optional<ResultCheckpoint>{});
    ResourceAllocationScope optional(resources_);
    try {
      ResourceString key(unit.content_key + "/checkpoint",
                         ResourceAllocator<char>(resources_));
      auto scope = cache_state_.scope(key, checkpoints_,
                                      options_.dependencies.sets.maximum_boxes);
      auto found = scope ? scope->find(phase, before) : ResultCheckpoint{};
      if (!found.valid())
        return Answer(std::optional<ResultCheckpoint>{});
      auto witness = std::static_pointer_cast<const ResultCheckpointWitness>(
          found.witness_);
      // The scope is a content identity, shared by equivalent calls. The
      // current instruction already supplied its own fixed dependency proof;
      // restoring state must not import an earlier call's physical tile ID.
      if (!witness || witness->scope != key || found.phase() != phase ||
          found.sequence() > before || !found.state().owned_by(resources_))
        return Answer(protocol_failure("static checkpoint identity mismatch"));
      if (!cache_work(witness->weight).ok())
        return cache_failure_.ok() ? Answer(std::optional<ResultCheckpoint>{})
                                   : Answer(cache_failure_);
      lock.unlock();
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_checkpoint_borrowed();
#endif
      return Answer(std::optional<ResultCheckpoint>{std::move(found)});
    } catch (const std::bad_alloc&) {
      return Answer(std::optional<ResultCheckpoint>{});
    }
  }
  Status checkpoint_publish(UnitState& unit,
                            const PlanTileInstruction& instruction,
                            std::uint32_t phase, std::uint64_t sequence,
                            const ResultRef& state) {
    std::unique_lock<std::recursive_mutex> lock(cache_mutex_);
    auto status = cache_allowed(unit);
    if (!status.ok())
      return status;
    if (!phase || !state.owned_by(resources_))
      return protocol_failure("invalid checkpoint state");
    auto facts = state.descriptor();
    if (!facts.ok())
      return facts.status();
    status = cache_proof(unit, instruction);
    if (!status.ok() || unit.content_key.empty() || !cache_state_.remaining())
      return status;
    ResourceAllocationScope optional(resources_);
    try {
      ResourceString key(unit.content_key + "/checkpoint",
                         ResourceAllocator<char>(resources_));
      auto weight = std::uint64_t{1} + key.size() + unit.record.inputs.size();
      for (const auto& input : unit.record.inputs) {
        const auto intervals = input.tiles.intervals().size();
        if (intervals > UINT64_MAX - weight)
          return Status::success();
        weight += intervals;
      }
      if (weight > options_.dependencies.sets.maximum_boxes)
        return Status::success();
      if (!cache_work(weight).ok())
        return cache_failure_;
      auto scope = cache_state_.scope(key, checkpoints_,
                                      options_.dependencies.sets.maximum_boxes);
      if (!scope)
        return Status::success();
      auto witness = std::allocate_shared<ResultCheckpointWitness>(
          ResourceAllocator<ResultCheckpointWitness>(resources_), resources_);
      witness->tiles = unit.record;
      witness->scope = std::move(key);
      witness->weight = weight;
      ResultCheckpoint checkpoint;
      checkpoint.phase_ = phase;
      checkpoint.sequence_ = sequence;
      checkpoint.state_ = state;
      checkpoint.witness_ = std::move(witness);
      if (scope->put(checkpoint, weight)) {
        lock.unlock();
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_checkpoint_published();
#endif
      }
      return Status::success();
    } catch (const std::bad_alloc&) {
      return Status::success();
    }
  }
  Result<ResultRef> evaluate_block(
      UnitState& unit, const PlanTileInstruction& instruction,
      const ResultProgramPhase& phase, std::uint32_t kind, std::uint64_t begin,
      std::uint64_t end, std::uint64_t mode, const ResultRef& incoming,
      const std::function<Result<ResultRef>()>& compute,
      const std::function<std::uint64_t()>& gpu_dispatches) {
    using Answer = Result<ResultRef>;
    std::unique_lock<std::recursive_mutex> lock(cache_mutex_);
    auto status = cache_allowed(unit);
    if (!status.ok())
      return Answer(status);
    if (!kind || begin >= end || !compute)
      return Answer(protocol_failure("invalid block request"));
    status = result_block_state(incoming, resources_);
    if (!status.ok())
      return Answer(status);
    std::string key;
    if (cache_ && cache_->enabled()) {
      status = cache_proof(unit, instruction, true);
      if (!status.ok())
        return Answer(status);
      ResourceAllocationScope optional(resources_);
      try {
        if (!unit.block_content_key.empty() && cache_state_.remaining()) {
          content_internal::Sha256 hash;
          hash.text("photospider.static-block.v1");
          hash.text(unit.block_content_key);
          hash.integer(kind);
          hash.integer(begin);
          hash.integer(end);
          hash.integer(mode);
          hash.text(incoming.schema().canonical());
          auto facts = incoming.descriptor();
          if (!facts.ok())
            return Answer(facts.status());
          status = append_block_samples(
              &hash, facts.value().tensor_coverage(0),
              incoming.schema().tensors[0].descriptor.element_type,
              [&](const auto& at, void* data, std::size_t bytes) {
                auto checked = cache_work(1);
                return checked.ok()
                           ? incoming.read_tensor(facts.value(), 0, at, data,
                                                  bytes, cancellation_)
                           : checked;
              },
              [&](auto n) { return cache_work(n); }, cancellation_);
          if (status.ok())
            key = hash.finish();
          else if (!cache_failure_.ok())
            return Answer(cache_failure_);
        }
        if (!key.empty()) {
          auto cached = cache_->get(key, cache_epoch_);
          if (cached.valid()) {
            auto reused = unpack_block_state(cached, incoming.schema(), phase);
            if (reused.ok()) {
              ++diagnostics_.block_cache_hits;
              return reused;
            }
            if (!optional_capacity(reused.status()))
              return reused;
          }
        }
      } catch (const std::bad_alloc&) {
        key.clear();
      }
    }
    ++diagnostics_.block_cache_misses;
    // The synchronous callback may dispatch CPU workers which use another
    // cache service. Do not hold metadata locks while waiting on user code.
    const auto before = gpu_dispatches ? gpu_dispatches() : 0;
    lock.unlock();
    auto result = core_internal::invoke_callback(compute);
    lock.lock();
    if (!result.ok())
      return result;
    status = cache_allowed(unit);
    if (!status.ok())
      return Answer(status);
    status = result_block_state(result.value(), resources_);
    if (!status.ok())
      return Answer(status);
    if (!result.value().schema().same_schema(incoming.schema()))
      return Answer(protocol_failure("computed block state schema mismatch"));
    const bool native = unit.query.backend == Backend::Gpu;
    if (!key.empty() &&
        (!native || (gpu_dispatches && gpu_dispatches() > before))) {
      ResourceAllocationScope optional(resources_);
      try {
        auto count = result.value().schema().tensors[0].sample_count();
        if (!count.ok() || count.value() > UINT64_MAX / 9)
          return result;
        status = cache_work(count.value() * 9);
        if (!status.ok())
          return cache_failure_.ok() ? result : Answer(cache_failure_);
        auto cache_phase = phase;
        cache_phase.consume_work = [&](auto n) { return cache_work(n); };
        auto packed = pack_block_state(result.value(), cache_phase);
        if (packed.ok())
          cache_->put(key, packed.value(), cache_epoch_, native);
        else if (!optional_capacity(packed.status()))
          return Answer(packed.status());
      } catch (const std::bad_alloc&) {
      }
    }
    return cache_failure_.ok() ? result : Answer(cache_failure_);
  }
  static bool optional_capacity(const Status& status) {
    return status.code == ErrorCode::ResourceExhausted &&
           (status.reason == FailureReason::None ||
            status.reason == FailureReason::CapacityLimit);
  }
  Status reuse(UnitState& unit, const PlanTileInstruction& instruction) {
    if (!cache_ || !cache_->enabled() || !unit.cacheable)
      return Status::success();
    std::lock_guard<std::recursive_mutex> lock(cache_mutex_);
    auto status = cache_proof(unit, instruction);
    if (!status.ok() || unit.content_key.empty())
      return status;
    ResourceAllocationScope optional(resources_);
    try {
      for (const auto& candidate :
           cache_->structured_candidates(unit.content_key, cache_epoch_)) {
        if (candidate->backend != unit.query.backend)
          continue;
        auto rebound = candidate->result.rebind_cached(
            unit.query.semantic_key, cancellation_,
            [&](auto n) { return cache_work(n); });
        if (!rebound.ok()) {
          if (!cache_failure_.ok())
            return cache_failure_;
          if (optional_capacity(rebound.status()))
            continue;
          return rebound.status();
        }
        ResultPublication publication{rebound.value(), true};
        status = validate_publication(unit, publication);
        if (!status.ok()) {
          if (!cache_failure_.ok())
            return cache_failure_;
          if (optional_capacity(status))
            continue;
          return status;
        }
        std::shared_ptr<plugin_internal::ResultPayloadBound> ledger;
        if (unit.payload_bound) {
          ResultProgramPhase phase{unit.query,
                                   unit.objects,
                                   resources_.allocator(),
                                   resources_,
                                   [&](auto n) { return cache_work(n); },
                                   {},
                                   {}};
          phase.tensors = &unit.tensors;
          phase.shared_stages = &unit.shared;
          auto staged = unit.payload_bound->candidate(phase);
          if (!staged.ok())
            continue;
          ledger = staged.take_value();
          status = ledger->capture(phase);
          if (status.ok())
            status = ledger->check(rebound.value(), phase);
          if (!status.ok()) {
            if (!cache_failure_.ok())
              return cache_failure_;
            continue;
          }
        }
        if (!cache_->structured_verified(candidate))
          continue;
        if (ledger)
          unit.payload_bound->commit_candidate(*ledger);
        unit.published = rebound.take_value();
        unit.raw_publication = unit.published;
        unit.complete = true;
        unit.reused = true;
        ++diagnostics_.cache_hits;
        return Status::success();
      }
      return cache_failure_;
    } catch (const std::bad_alloc&) {
      return Status::success();
    }
  }
  Status retain(StoredTile& tile, Backend backend) {
    if (!cache_ || tile.cache_key.empty())
      return Status::success();
    std::lock_guard<std::recursive_mutex> lock(cache_mutex_);
    ResourceAllocationScope optional(resources_);
    try {
      const auto weight =
          sizeof(StructuredCacheManifest) + 4 * (tile.cache_key.size() + 1);
      if (weight > cache_->dependency_metadata_limit() ||
          !cache_work(weight).ok())
        return cache_failure_;
      auto made = std::allocate_shared<StructuredCacheManifest>(
          ResourceAllocator<StructuredCacheManifest>(resources_));
      made->metadata = weight;
      made->epoch = cache_epoch_;
      made->backend = backend;
      made->key = ResourceString("static/" + tile.cache_key,
                                 ResourceAllocator<char>(resources_));
      made->content =
          ResourceString(tile.cache_key, ResourceAllocator<char>(resources_));
      made->result = tile.result;
      cache_->put_structured(tile.cache_key, std::move(made),
                             [&](auto n) { return cache_work(n); });
      tile.cache_key.clear();
      return cache_failure_;
    } catch (const std::bad_alloc&) {
      return Status::success();
    }
  }
  Status validate_need(const PlanTileInstruction& instruction,
                       const ResultProgramNeed& need) {
    const auto count = need.tiles.size() + need.shared_stages.size();
    if (!count ||
        (!need.tiles.empty() &&
         !need.tiles.get_allocator().owned_by(resources_)) ||
        (!need.shared_stages.empty() &&
         !need.shared_stages.get_allocator().owned_by(resources_)))
      return protocol_failure("invalid static capability request envelope");
    if (count > std::min<std::uint64_t>(
                    65536, options_.dependencies.sets.maximum_boxes))
      return {ErrorCode::ResourceExhausted, "static capability request limit"};
    auto charged = consume(count);
    if (!charged.ok())
      return charged;
    std::vector<std::uint32_t> seen;
    // This is a protocol check against static capabilities, not dependency
    // discovery. No producer lookup, coordinate conversion or task creation.
    for (const auto& requested : need.tiles) {
      if (!requested.tiles.owned_by(resources_))
        return protocol_failure("foreign input tile capability");
      auto valid = requested.validate();
      if (!valid.ok())
        return valid;
      const auto& step = plan_.steps()[instruction.step_index];
      if (requested.input >= step.inputs.size())
        return protocol_failure("invalid static input ordinal");
      if (!requested.descriptor_only) {
        const auto& grids = plan_.input_grids(step.inputs[requested.input]);
        if (requested.tensor_slot >= grids.size())
          return protocol_failure("invalid static tensor ordinal");
        if (requested.tiles.empty())
          continue;
      }
      bool covered = false;
      for (const auto& granted : instruction.inputs)
        if (granted.input == requested.input &&
            (requested.descriptor_only ||
             (!granted.descriptor_only &&
              granted.tensor_slot == requested.tensor_slot &&
              granted.tiles.contains(requested.tiles))))
          covered = true;
      if (!covered)
        return protocol_failure(
            "operation requested input outside its static mapping");
    }
    for (const auto& requested : need.shared_stages) {
      if (std::find(seen.begin(), seen.end(), requested.stage) != seen.end())
        return protocol_failure("duplicate shared capability request");
      seen.push_back(requested.stage);
      const auto& stages = plan_.steps()[instruction.step_index].shared_stages;
      if (requested.stage >= stages.size())
        return protocol_failure("invalid static shared ordinal");
      if (requested.tiles)
        for (const auto& member : requested.tiles->members)
          if (member.first >= stages[requested.stage]->tile_grids.size() ||
              !member.second.owned_by(resources_))
            return protocol_failure("invalid shared tile capability");
      if (requested.tiles && requested.tiles->empty())
        continue;
      bool covered = false;
      for (const auto& granted : instruction.shared_inputs) {
        if (granted.stage != requested.stage)
          continue;
        if (requested.tiles) {
          covered = granted.tiles.contains(*requested.tiles);
        } else {
          const auto& stage = *plan_.steps()[instruction.step_index]
                                   .shared_stages[requested.stage];
          covered = granted.tiles.members.size() == stage.tile_grids.size();
          for (std::uint32_t slot = 0;
               covered && slot < stage.tile_grids.size(); ++slot) {
            auto found = granted.tiles.members.find(slot);
            covered =
                found != granted.tiles.members.end() &&
                found->second.size() == stage.tile_grids[slot].tile_count();
          }
        }
      }
      if (!covered)
        return protocol_failure(
            "operation requested shared data outside its static mapping");
    }
    return Status::success();
  }
  Status validate_publication(UnitState& unit,
                              const ResultPublication& publication) {
    struct Services final : PublicationValidationServices {
      Services(StructuredExecution& owner, UnitState& unit)
          : owner(owner), unit(unit) {}
      Status operation_work(std::uint64_t n) override {
        return owner.work(unit, n);
      }
      Status run_work(std::uint64_t n) override { return owner.consume(n); }
      ErrorCode stop() override { return owner.stopped(); }
      CancellationToken cancellation() override { return owner.cancellation_; }
      FootprintLimits set_limits() override {
        return owner.options_.dependencies.sets;
      }
      StructuredExecution& owner;
      UnitState& unit;
    } services(*this, unit);
    PublicationValidationView view{unit.contract,
                                   unit.query,
                                   unit.raw_publication,
                                   unit.revision,
                                   unit.contract.node_id,
                                   resources_,
                                   options_.maximum_result_window_bytes,
                                   services,
                                   true};
    return ResultPublicationValidator::validate(view, publication);
  }
  void prepare_timing() {
    auto& timings = diagnostics_.operation_timings;
    if (timings.size() == timings.capacity()) {
      if (timings.size() == timings.max_size())
        throw std::bad_alloc();
      const auto extra = std::min(std::max<std::size_t>(timings.capacity(), 1),
                                  timings.max_size() - timings.size());
      timings.reserve(timings.size() + extra);
    }
  }
  Status count_samples(OperationTiming& timing, const Footprint& samples) {
    auto count = samples.element_count();
    if (!count.ok() && count.status().code != ErrorCode::ResourceExhausted)
      return count.status();
    // Cardinality overflow is diagnostic saturation, not payload admission.
    if (!count.ok() || count.value() > UINT64_MAX - timing.computed_elements) {
      timing.computed_elements = UINT64_MAX;
      timing.computed_elements_saturated = true;
    } else {
      timing.computed_elements += count.value();
    }
    return Status::success();
  }
  Status certify_shared(UnitState& unit, const ResultPublication& publication,
                        const ResultDescriptor& raw) {
    auto produced = plugin_internal::published_tiles(
        *unit.contract.output_result_schema, raw, plan_.tile_extent(),
        resources_, options_.dependencies.sets);
    if (!produced.ok())
      return produced.status();
    auto visible = plugin_internal::visible_tiles(
        *unit.contract.output_result_schema, produced.value(),
        plan_.tile_extent(), resources_);
    if (!visible.ok())
      return visible.status();
    const bool complete =
        unit.query.prepared->requires_complete_stage_validation(
            unit.query.shared_stage);
    if (complete) {
      bool all = true;
      for (std::uint32_t member = 0; member < unit.contract.tile_grids.size();
           ++member) {
        auto found = produced.value().members.find(member);
        all = all && found != produced.value().members.end() &&
              found->second.size() ==
                  unit.contract.tile_grids[member].tile_count();
      }
      if (!all) {
        if (publication.complete)
          return protocol_failure("incomplete shared validation barrier");
        visible = Result<TileRequest>(TileRequest{});
      }
    }
    if (publication.complete && visible.value() != produced.value())
      return protocol_failure("incomplete shared publication scope");
    TileRequest delta;
    for (const auto& member : visible.value().members) {
      auto previous = unit.visible_tiles.members.find(member.first);
      auto added = previous == unit.visible_tiles.members.end()
                       ? Result<TileSet>(member.second)
                       : member.second.subtract(previous->second, resources_);
      if (!added.ok())
        return added.status();
      if (!added.value().empty())
        delta.members.emplace(member.first, added.take_value());
    }
    if (!unit.visible_builder) {
      auto builder = ResultBuilder::start(
          resources_, *unit.contract.output_result_schema,
          unit.query.semantic_key, {}, plan_.tile_extent());
      if (!builder.ok())
        return builder.status();
      unit.visible_builder = builder.take_value();
    }
    auto previous = unit.visible_builder->reference().descriptor(false);
    if (!previous.ok())
      return previous.status();
    for (const auto& member : delta.members) {
      auto samples = unit.contract.tile_grids[member.first].coverage(
          member.second, options_.dependencies.sets);
      if (!samples.ok())
        return samples.status();
      for (const auto& region : samples.value().boxes()) {
        auto status = plugin_internal::append_publication_view(
            *unit.visible_builder, publication.result, raw, member.first,
            region, resources_, cancellation_,
            [&](auto n) { return work(unit, n); });
        if (!status.ok())
          return status;
      }
      auto counted =
          count_samples(diagnostics_.operation_timings.back(), samples.value());
      if (!counted.ok())
        return counted;
    }
    if (!delta.empty()) {
      auto exposed = unit.visible_builder->reference();
      auto facts = exposed.descriptor(false);
      if (!facts.ok())
        return facts.status();
      auto status = plugin_internal::validate_result_contract(
          *unit.query.prepared, true, unit.query.output_index, exposed,
          facts.value(), complete ? nullptr : &previous.value(), resources_,
          cancellation_, [&](auto n) { return work(unit, n); },
          options_.maximum_result_window_bytes, ErrorCode::OperationFailed,
          unit.query.shared_stage);
      if (!status.ok())
        return status;
    }
    unit.visible_tiles = visible.take_value();
    if (!unit.visible_tiles.empty()) {
      auto exposed = publication.complete
                         ? unit.visible_builder->seal()
                         : unit.visible_builder->reference().capture();
      if (!exposed.ok())
        return exposed.status();
      unit.published = exposed.take_value();
    }
    return Status::success();
  }
  bool retry_cpu(UnitState& unit, const PlanTileInstruction& instruction,
                 const Status& failed) {
    if (!instruction.cpu_fallback || unit.query.backend != Backend::Gpu ||
        unit.fallback_tainted || unit.raw_publication.valid() ||
        stopped() != ErrorCode::Ok ||
        !plugin_internal::FailureLatch::retryable_backend_failure(failed) ||
        !unit.latch->backend_retry_allowed() ||
        (unit.failure->load() != ErrorCode::Ok &&
         unit.failure->load() != ErrorCode::BackendUnavailable))
      return false;
    auto failure = std::allocate_shared<std::atomic<ErrorCode>>(
        ResourceAllocator<std::atomic<ErrorCode>>(resources_), ErrorCode::Ok);
    auto latch = std::allocate_shared<plugin_internal::FailureLatch>(
        ResourceAllocator<plugin_internal::FailureLatch>(resources_));
    unit.continuation = {};
    unit.failure = std::move(failure);
    unit.latch = std::move(latch);
    unit.fallback_tainted = true;
    unit.pure = false;
    unit.cacheable = false;
    unit.content_key.clear();
    unit.block_content_key.clear();
    unit.proof_ready = false;
    unit.block_proof_ready = false;
    unit.query.backend = Backend::Cpu;
    unit.key += "/cpu-fallback";
    unit.query.semantic_key = unit.key;
    unit.started = false;
    diagnostics_.fallback_reasons.push_back(failed.message);
    return true;
  }
  Status execute(const PlanTileInstruction& instruction) {
    auto status = consume(1);
    if (!status.ok())
      return status;
    const auto& original = plan_.steps().at(instruction.step_index);
    const auto& contract =
        instruction.shared_stage == UINT32_MAX
            ? original
            : *original.shared_stages.at(instruction.shared_stage);
    std::shared_ptr<UnitState> local;
    UnitState* unit = nullptr;
    if (instruction.shared_stage == UINT32_MAX) {
      local = std::allocate_shared<UnitState>(
          ResourceAllocator<UnitState>(resources_), contract, resources_);
      unit = local.get();
      auto& saved = values_[instruction.step_index];
      const auto bound =
          contract.traits.outputs[0].maximum_output_payload_bytes;
      if (bound && !saved.payload_bound)
        saved.payload_bound =
            std::allocate_shared<plugin_internal::ResultPayloadBound>(
                ResourceAllocator<plugin_internal::ResultPayloadBound>(
                    resources_),
                *bound, true);
      unit->payload_bound = saved.payload_bound;
    } else {
      auto& shared = shared_[{contract.node_id, instruction.shared_stage}];
      if (!shared)
        shared = std::allocate_shared<UnitState>(
            ResourceAllocator<UnitState>(resources_), contract, resources_);
      unit = shared.get();
    }
    unit->query.tensor_slot = instruction.tensor_slot;
    unit->query.tile_unit = instruction.tile_unit;
    unit->query.tensor_outputs = instruction.tensor_outputs;
    auto start_attempt = [&]() -> Status {
      for (;;) {
        // Reserve before entering plugin code: a failing callback may retain
        // all remaining Host capacity until its continuation is retired.
        prepare_timing();
        const auto began = std::chrono::steady_clock::now();
        const auto backend = unit->query.backend;
        auto started = dispatch(
            Backend::Cpu, false, [&](const StructuredServices& services) {
              if (unit->query.backend == Backend::Gpu &&
                  !services.native_gpu_available) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
                execution_testing::notify_before_scheduler_failure(
                    execution_testing::SchedulerFailurePoint::
                        GpuBackendUnavailable);
#endif
                return Status{ErrorCode::BackendUnavailable,
                              "compiled GPU backend is unavailable"};
              }
              ErrorCode sticky = ErrorCode::Ok;
              ResourceAllocationScope scope(resources_, &sticky);
              auto allocator = resources_.allocator().limited(
                  std::min(contract.traits.outputs[0].continuation_bytes,
                           options_.dependencies.maximum_state_bytes),
                  [latch = unit->latch](const Status& failure) {
                    latch->record(failure);
                  });
              auto created = operations_->start_result_compiled(
                  contract.operation, unit->query, allocator, unit->failure);
              if (sticky != ErrorCode::Ok)
                unit->latch->record(Status{sticky, {}});
              if (!unit->latch->snapshot().ok())
                return unit->latch->snapshot();
              if (!created.ok())
                return created.status();
              unit->continuation = created.take_value();
              return Status::success();
            });
        started = started.ok() ? unit->latch->snapshot()
                               : unit->latch->record(started);
        if (started.ok()) {
          unit->started = true;
          return started;
        }
        OperationTiming timing{original.result_ref(), backend, micros(began),
                               started.code,          1,       0};
        timing.shared_stage = instruction.shared_stage;
        diagnostics_.operation_timings.push_back(std::move(timing));
        if (!retry_cpu(*unit, instruction, started))
          return started;
      }
    };
    if (!unit->started) {
      unit->record.unit = {instruction.step_index, contract.output_index,
                           instruction.tensor_slot, instruction.tile_unit};
      unit->query.prepared = contract.prepared;
      unit->query.output_index = contract.output_index;
      unit->query.shared_stage = instruction.shared_stage;
      unit->query.tensor_slot = instruction.tensor_slot;
      unit->query.tile_unit = instruction.tile_unit;
      unit->query.tensor_outputs = instruction.tensor_outputs;
      unit->query.tile_extent = plan_.tile_extent();
      unit->query.backend = contract.backend;
      unit->query.page_bytes = options_.maximum_result_window_bytes;
      unit->query.cancellation = cancellation_;
      unit->query.resources = selected_resources_;
      unit->key = key(std::to_string(contract.node_id) + "/" +
                      std::to_string(instruction.shared_stage) + "/" +
                      std::to_string(contract.output_index) + "/" +
                      std::to_string(instruction.tensor_slot) + "/" +
                      std::to_string(instruction.tile_unit));
      unit->query.semantic_key = unit->key;
      unit->query.snapshot_identity = snapshot_;
      unit->remaining = instruction.shared_stage == UINT32_MAX
                            ? UINT64_MAX
                            : contract.prepared->traits()
                                  .shared_stages[instruction.shared_stage]
                                  .maximum_work;
      unit->discovery_remaining = options_.dependencies.maximum_work;
    }
    status = bind(*unit, instruction);
    if (!status.ok())
      return status;
    status = reuse(*unit, instruction);
    if (!status.ok())
      return status;
    auto satisfied = [&]() -> Result<bool> {
      if (!unit->published.valid())
        return Result<bool>(false);
      auto facts = unit->published.descriptor(false);
      if (!facts.ok())
        return Result<bool>(facts.status());
      for (std::uint32_t member = 0;
           member < instruction.publication_samples.size(); ++member) {
        auto outside = instruction.publication_samples[member].subtract(
            facts.value().tensor_coverage(member), options_.dependencies.sets);
        if (!outside.ok())
          return Result<bool>(outside.status());
        if (!outside.value().empty())
          return Result<bool>(false);
      }
      return Result<bool>(true);
    };
    for (;;) {
      auto ready = satisfied();
      if (!ready.ok())
        return ready.status();
      if (ready.value())
        break;
      if (unit->complete)
        return protocol_failure("shared stage omitted a planned publication");
      if (!unit->started) {
        const auto failure = unit->failure;
        status = start_attempt();
        if (!status.ok())
          return status;
        if (unit->failure != failure) {
          status = bind(*unit, instruction);
          if (!status.ok())
            return status;
        }
      }
      if (++unit->polls >
          std::min<std::uint64_t>(
              options_.dependencies.maximum_stages,
              contract.traits.outputs[0].maximum_dependency_stages))
        return {ErrorCode::ResourceExhausted, "static unit phase limit"};
      status = work(*unit, 1);
      if (!status.ok())
        return status;
      Result<ResultProgramPoll> polled(Status{ErrorCode::Internal, {}});
      prepare_timing();
      const auto began = std::chrono::steady_clock::now();
      OperationTiming timing{
          original.result_ref(), unit->query.backend, 0, ErrorCode::Ok, 1, 0};
      timing.shared_stage = instruction.shared_stage;
      if (!contract.prepared->diagnostic_definitions().empty()) {
        timing.reports.emplace(resources_);
        status = timing.reports->initialize(
            contract.prepared->diagnostic_definitions());
        if (!status.ok())
          return status;
      }
      status = dispatch(
          unit->query.backend, contract.traits.cpu_staged_tiles,
          [&](const StructuredServices& services) {
            ErrorCode sticky = ErrorCode::Ok;
            ResourceAllocationScope scope(resources_, &sticky);
            auto observe = [&](const Status& failure) {
              if (!failure.ok()) {
                unit->latch->record(failure);
                auto expected = ErrorCode::Ok;
                unit->failure->compare_exchange_strong(expected, failure.code);
              }
            };
            auto allocator = services.allocator.limited(
                contract.traits.workspace_bytes, observe);
            ResultProgramPhase phase{unit->query,
                                     unit->objects,
                                     allocator,
                                     resources_,
                                     [&](auto n) { return work(*unit, n); },
                                     unit->failure,
                                     observe};
            phase.failure_latch = unit->latch;
            phase.tensors = &unit->tensors;
            phase.descriptors = &unit->descriptors;
            phase.shared_stages = &unit->shared;
            phase.cpu_parallel = services.cpu_parallel;
            phase.cpu_tiles = services.cpu_tiles;
            phase.gpu = services.gpu;
            phase.gpu_status = services.gpu_status;
            phase.report_diagnostic = [&](std::uint32_t type, ByteView bytes) {
              auto checked = work(*unit, 1);
              if (checked.ok())
                checked =
                    timing.reports
                        ? timing.reports->report(type, bytes)
                        : protocol_failure("undeclared operation diagnostic");
              observe(checked);
              return checked;
            };
            auto guarded_service = [&](auto&& callback) {
              using Answer = decltype(callback());
              auto result = [&]() -> Answer {
                try {
                  return core_internal::invoke_callback(callback);
                } catch (const std::bad_alloc&) {
                  return Answer(Status{ErrorCode::ResourceExhausted, {}});
                } catch (...) {
                  return Answer(Status{ErrorCode::OperationFailed, {}});
                }
              }();
              if constexpr (std::is_same_v<Answer, Status>)
                observe(result);
              else if (!result.ok())
                observe(result.status());
              return result;
            };
            phase.checkpoint_before = [&](std::uint32_t kind,
                                          std::uint64_t before) {
              unit->latch->veto_backend_retry();
              return guarded_service(
                  [&]() -> Result<std::optional<ResultCheckpoint>> {
                    if (unit->in_discovery.load())
                      return Result<std::optional<ResultCheckpoint>>(
                          protocol_failure("checkpoint inside GPU selection"));
                    return checkpoint_before(*unit, instruction, kind, before);
                  });
            };
            phase.checkpoint_publish = [&](std::uint32_t kind,
                                           std::uint64_t sequence,
                                           const ResultRef& state) {
              unit->latch->veto_backend_retry();
              return guarded_service([&] {
                if (unit->in_discovery.load())
                  return protocol_failure("checkpoint inside GPU selection");
                return checkpoint_publish(*unit, instruction, kind, sequence,
                                          state);
              });
            };
            std::atomic<bool> in_block{false};
            phase.block =
                [&](std::uint32_t kind, std::uint64_t begin, std::uint64_t end,
                    std::uint64_t mode, const ResultRef& incoming,
                    const std::function<Result<ResultRef>()>& compute) {
                  unit->latch->veto_backend_retry();
                  return guarded_service([&]() -> Result<ResultRef> {
                    bool idle = false;
                    if (unit->in_discovery.load() ||
                        !in_block.compare_exchange_strong(idle, true))
                      return Result<ResultRef>(
                          protocol_failure("nested or overlapping pure block"));
                    struct Leave {
                      std::atomic<bool>& active;
                      ~Leave() { active.store(false); }
                    } leave{in_block};
                    return evaluate_block(*unit, instruction, phase, kind,
                                          begin, end, mode, incoming, compute,
                                          services.gpu_dispatches);
                  });
                };
            const auto entry_thread = std::this_thread::get_id();
            phase.discover =
                [&](std::uint32_t capacity, std::uint32_t candidates,
                    const std::function<Status(const ResultGpuRequestTable&)>&
                        compute) {
                  using Answer =
                      Result<std::shared_ptr<const ResultDiscoveryReceipt>>;
                  return guarded_service([&]() -> Answer {
                    if (std::this_thread::get_id() != entry_thread ||
                        in_block.load() ||
                        unit->query.backend != Backend::Gpu || !phase.gpu ||
                        !services.gpu_dispatches)
                      return Answer(protocol_failure(
                          "invalid static GPU selection scope"));
                    bool idle = false;
                    if (!unit->in_discovery.compare_exchange_strong(idle, true))
                      return Answer(protocol_failure("nested GPU selection"));
                    struct Leave {
                      std::atomic<bool>& active;
                      ~Leave() { active.store(false); }
                    } leave{unit->in_discovery};
                    unit->latch->veto_backend_retry();
                    const auto before = services.gpu_dispatches();
                    auto decoded = ResultNativeScope::discover(
                        phase.query, capacity, candidates, compute,
                        [&](auto n) { return work(*unit, n); });
                    if (!decoded.ok())
                      return Answer(decoded.status());
                    if (services.gpu_dispatches() == before)
                      return Answer(
                          Status{ErrorCode::OperationFailed,
                                 "GPU selection submitted no native work"});
                    for (const auto& selected : decoded.value()) {
                      bool covered = false;
                      for (const auto& grant : instruction.inputs) {
                        auto charged = work(*unit, 1);
                        if (!charged.ok())
                          return Answer(charged);
                        if (selected.input == grant.input &&
                            (selected.descriptor_only ||
                             (!grant.descriptor_only &&
                              selected.tensor_slot == grant.tensor_slot &&
                              grant.tiles.contains(selected.tiles)))) {
                          covered = true;
                          break;
                        }
                      }
                      if (!covered)
                        return Answer(protocol_failure(
                            "GPU selection exceeds compiled input grants"));
                    }
                    auto receipt = std::allocate_shared<ResultDiscoveryReceipt>(
                        ResourceAllocator<ResultDiscoveryReceipt>(resources_));
                    receipt->tiles = decoded.take_value();
                    return Answer(std::move(receipt));
                  });
                };
            ResultGpuService gpu(phase.gpu, phase.gpu_status, observe);
            phase.gpu = gpu.get();
            if (unit->payload_bound) {
              auto admitted = unit->payload_bound->capture(phase);
              if (!admitted.ok())
                return admitted;
            }
            polled = unit->continuation.poll(phase);
            if (polled.ok() && unit->payload_bound) {
              if (const auto* publication =
                      std::get_if<ResultPublication>(&polled.value())) {
                auto admitted = unit->payload_bound->capture(phase);
                if (admitted.ok())
                  admitted =
                      unit->payload_bound->check(publication->result, phase);
                if (!admitted.ok())
                  return admitted;
              }
            }
            if (services.gpu_status) {
              auto failure = services.gpu_status();
              if (failure.code == ErrorCode::InvalidArgument) {
                failure.reason = FailureReason::UnauthorizedRead;
                failure.detail.origin = FailureOrigin::Protocol;
                failure.detail.scope = FailureScope::Group;
              }
              observe(failure);
            }
            if (services.gpu_dispatches)
              timing.native_dispatch_count = services.gpu_dispatches();
            if (timing.native_dispatch_count)
              unit->latch->veto_backend_retry();
            if (!unit->latch->snapshot().ok())
              return unit->latch->snapshot();
            if (sticky != ErrorCode::Ok)
              return Status{sticky, {}};
            if (unit->failure->load() != ErrorCode::Ok)
              return Status{unit->failure->load(), {}};
            return polled.ok() ? Status::success() : polled.status();
          });
      // Dispatch retirement also checks native/CPU services. Preserve the
      // first observed host failure while recording later retry vetoes.
      status =
          status.ok() ? unit->latch->snapshot() : unit->latch->record(status);
      timing.duration_us = micros(began);
      timing.outcome = status.code;
      diagnostics_.operation_timings.push_back(std::move(timing));
      if (!status.ok()) {
        if (!retry_cpu(*unit, instruction, status))
          return status;
        status = start_attempt();
        if (status.ok())
          status = bind(*unit, instruction);
        if (!status.ok())
          return status;
        continue;
      }
      if (const auto* need = std::get_if<ResultProgramNeed>(&polled.value())) {
        status = validate_need(instruction, *need);
        if (!status.ok())
          return status;
        continue;
      }
      const auto& publication = std::get<ResultPublication>(polled.value());
      status = validate_publication(*unit, publication);
      if (!status.ok())
        return status;
      auto captured = publication.result.capture();
      if (!captured.ok())
        return captured.status();
      unit->raw_publication = captured.take_value();
      auto facts = unit->raw_publication.descriptor(false);
      if (!facts.ok())
        return facts.status();
      if (instruction.shared_stage == UINT32_MAX) {
        unit->published = unit->raw_publication;
      } else {
        status = certify_shared(*unit, publication, facts.value());
        if (!status.ok())
          return status;
      }
      unit->revision = facts.value().revision();
      unit->complete = publication.complete;
    }
    if (instruction.shared_stage == UINT32_MAX) {
      auto evidence = capture_record(unit->record);
      if (!evidence.ok())
        return evidence.status();
      status = records_->install(0, unit->record, unit->published, nullptr,
                                 nullptr, [&](auto n) { return consume(n); });
      if (!status.ok())
        return status;
      auto& saved = values_[instruction.step_index];
      saved.assembled = {};
      saved.cacheable &= unit->cacheable;
      saved.pure &= unit->pure;
      saved.tiles.push_back({unit->published, &instruction.publication_samples,
                             evidence.take_value(),
                             unit->reused || !unit->cacheable
                                 ? std::string{}
                                 : unit->content_key});
      if (!unit->reused) {
        for (const auto& samples : instruction.publication_samples) {
          auto& timing = diagnostics_.operation_timings.back();
          auto counted = count_samples(timing, samples);
          if (!counted.ok())
            return counted;
        }
      }
      diagnostics_.selected_backends[original.result_ref()] =
          unit->query.backend;
      // An independent certified tile is observable immediately, including
      // cache hits. This commits the current static instruction only. Bundle,
      // prefix and full-validator outputs keep their aggregate barrier.
      const auto& scopes = original.output_result_schema->publication_scopes;
      if (subscription_.has_callback() &&
          !original.prepared->requires_complete_validation(
              true, original.output_index) &&
          std::all_of(scopes.begin(), scopes.end(), [](const auto& scope) {
            return scope.policy == ResultPublicationPolicy::IndependentChunks;
          })) {
        auto facts = unit->published.descriptor();
        if (!facts.ok())
          return facts.status();
        status = plugin_internal::validate_result_contract(
            *original.prepared, true, original.output_index, unit->published,
            facts.value(), nullptr, resources_, cancellation_,
            [&](auto n) { return consume(n); },
            options_.maximum_result_window_bytes, ErrorCode::OperationFailed);
        if (!status.ok())
          return status;
        status = retain(saved.tiles.back(), unit->query.backend);
        if (!status.ok())
          return status;
        saved.individually_certified = true;
        status =
            subscription_.notify(instruction.step_index, original.result_ref(),
                                 unit->published, facts.value().revision());
        if (!status.ok())
          return status;
      }
    }
    return consume(0);
  }
  Status digest(const ExecutionResult& result) {
    std::uint64_t samples = 0;
    for (const auto& named : result.results) {
      auto facts = named.second.descriptor();
      if (!facts.ok())
        return facts.status();
      for (std::uint32_t member = 0; member < facts.value().tensor_count();
           ++member) {
        auto count = facts.value().tensor_coverage(member).element_count();
        if (!count.ok() ||
            count.value() >
                options_.maximum_result_digest_samples -
                    std::min(samples, options_.maximum_result_digest_samples))
          return Status::success();
        samples += count.value();
      }
    }
    if (!options_.maximum_result_digest_samples || samples > remaining_.load())
      return Status::success();
    content_internal::Sha256 digest;
    digest.text("photospider.result-content.v1");
    for (const auto& named : result.results) {
      digest.text(named.first);
      digest.text(named.second.schema().canonical());
      auto facts = named.second.descriptor();
      if (!facts.ok())
        return facts.status();
      for (std::uint32_t member = 0; member < facts.value().tensor_count();
           ++member) {
        const auto& coverage = facts.value().tensor_coverage(member);
        digest.integer(coverage.boxes().size());
        for (const auto& box : coverage.boxes())
          for (const auto& axis : box.dimensions()) {
            digest.integer(axis.offset);
            digest.integer(axis.extent);
          }
        const auto bytes = TensorSpan::element_size(
            named.second.schema().tensors[member].descriptor.element_type);
        auto scanned = coverage.visit(
            [&](const auto& coordinate) {
              auto status = consume(1);
              if (!status.ok())
                return status;
              std::uint8_t data[8]{};
              status =
                  named.second.read_tensor(facts.value(), member, coordinate,
                                           data, bytes, cancellation_);
              if (status.ok())
                digest.bytes(data, bytes);
              return status;
            },
            options_.maximum_result_digest_samples, cancellation_);
        if (!scanned.ok())
          return scanned;
      }
    }
    diagnostics_.result_digest =
        ResourceString(digest.finish(), ResourceAllocator<char>(resources_));
    return Status::success();
  }

  std::shared_ptr<const ExecutionPlan> owner_;
  const ExecutionPlan& plan_;
  std::vector<ExecutionBinding> bindings_;
  std::shared_ptr<OperationRegistry> operations_;
  ResourceBudget resources_;
  ExecutionOptions options_;
  CancellationToken cancellation_;
  std::function<ErrorCode()> stop_;
  StructuredDispatch dispatch_;
  std::string snapshot_;
  ResultSubscription subscription_;
  std::atomic<std::uint64_t> remaining_{0};
  std::uint64_t activation_ = 0, run_identity_ = 0;
  ResourceSet selected_resources_;
  ResourceLease state_capacity_;
  std::vector<ValueState> values_;
  std::vector<ResultRef> arguments_, externals_;
  std::map<std::pair<IRCallId, std::uint32_t>, std::shared_ptr<UnitState>>
      shared_;
  TileRecord control_;
  std::vector<std::shared_ptr<const TileRecord>> argument_records_;
  std::shared_ptr<TileRecords> records_;
  std::recursive_mutex callback_mutex_;
  ExecutionDiagnostics diagnostics_;
  ResultCache* cache_;
  ResultCheckpoints* checkpoints_;
  std::uint64_t cache_epoch_;
  StructuredCacheState cache_state_;
  std::recursive_mutex cache_mutex_;
  Status cache_failure_;
  bool control_cacheable_ = true;
  bool control_pure_ = true;
};

Result<ExecutionResult> execute_structured(
    const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
    std::shared_ptr<OperationRegistry> operations, ResourceBudget resources,
    const ExecutionOptions& options, const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop, const StructuredDispatch& dispatch,
    const DemandQuery* requested, const std::string& snapshot_identity,
    ResultCheckpoints* checkpoints, ResultCache* cache,
    std::shared_ptr<const ExecutionPlan> plan_owner, std::uint32_t) {
  try {
    // Demand specialization must finish before entering the interpreter.
    if (requested)
      for (const auto& item : *requested)
        if (plan.output_requests().count(item.first) == 0 ||
            plan.output_requests().at(item.first) != item.second)
          return Result<ExecutionResult>(protocol_failure(
              "execution request requires a specialized plan"));
    StructuredExecution execution(
        plan, std::move(bindings), std::move(operations), std::move(resources),
        options, cancellation, stop, dispatch, snapshot_identity,
        std::move(plan_owner), checkpoints, cache);
    return execution.run();
  } catch (const std::bad_alloc&) {
    return Result<ExecutionResult>(
        {ErrorCode::ResourceExhausted, "static execution allocation failed"});
  } catch (...) {
    return Result<ExecutionResult>(
        {ErrorCode::OperationFailed, "static execution failed"});
  }
}
}  // namespace ps::execution_internal
