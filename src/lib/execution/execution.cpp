#include "photospider/execution/execution.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/callback_environment.hpp"
#include "core/resource_observation.hpp"
#include "data/result_device_access.hpp"
#include "data/result_identity.hpp"
#include "execution/block_memory.hpp"
#include "execution/cpu_workers.hpp"
#include "execution/diagnostics.hpp"
#include "execution/fair_admission.hpp"
#include "execution/native_gpu.hpp"
#include "execution/task_schedule.hpp"
#include "execution/tile_view.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/plugin/fixed_invocation.hpp"

namespace ps::execution_internal {
namespace {
using Id = physical::Id;
Status failure(ErrorCode code, const char* text) {
  return {code, text};
}
unsigned priority(const Status& status) {
  return status.ok()                           ? 0
         : status.code == ErrorCode::Cancelled ? 3
         : status.code == ErrorCode::Stale     ? 2
                                               : 1;
}
void copy_status(Status* target, const Status& source) {
  target->code = source.code;
  target->reason = source.reason;
  target->detail = source.detail;
  target->message.assign(source.message.data(),
                         std::min<std::size_t>(source.message.size(),
                                               PS_KERNEL_MESSAGE_CAPACITY_V8));
}
// Retirement moves this owner with the public record. Observer copies are
// caller-owned; the delivery lane retains this original through callback exit.
struct DeliveryRecord {
  DeliveryRecord() = default;
  explicit DeliveryRecord(const ResourceLease& metadata)
      : allocation(metadata) {
    record.status.message.reserve(PS_KERNEL_MESSAGE_CAPACITY_V8);
    allocation.commit(record.status.message.capacity() + 1);
  }
  DeliveryRecord(DeliveryRecord&&) noexcept = default;
  DeliveryRecord& operator=(DeliveryRecord&& other) noexcept {
    if (this != &other) {
      DeliveryRecord replaced(std::move(other));
      allocation.swap(replaced.allocation);
      std::swap(record, replaced.record);
      std::swap(queued_ns, replaced.queued_ns);
    }
    return *this;
  }
  core_internal::MetadataAllocation allocation;
  CallRecord record;
  std::uint64_t queued_ns = 0;
};
static_assert(std::is_nothrow_move_constructible_v<CallRecord> &&
              std::is_nothrow_move_assignable_v<CallRecord>);
static_assert(sizeof(std::string) < PS_KERNEL_MESSAGE_CAPACITY_V8);
struct TileOwner {
  std::shared_ptr<const CpuStorage> storage;
  std::shared_ptr<const gpu_internal::DeviceStorage> device;
  std::uint64_t offset = 0;
  TileOwner() = default;
  TileOwner(std::shared_ptr<const CpuStorage> value, std::uint64_t base)
      : storage(std::move(value)), offset(base) {}
  TileOwner(std::shared_ptr<const gpu_internal::DeviceStorage> value,
            std::uint64_t base)
      : device(std::move(value)), offset(base) {}
  const std::uint8_t* data() const { return storage->bytes().data() + offset; }
  const void* allocation() const { return device ? device->handle() : data(); }
};
struct ValueIdentity {
  ResourceLease metadata;
  // Kernel association; Imported values retain the external source ID.
  // Their canonical COPY envelope instead consumes its own reservation.
  std::uint64_t object = 0;
  Id original = physical::none;
  bool canonical_allowed = true;
  data_internal::ResultIdentity reservation;
  WeakResultRef canonical;
};
struct Incoming {
  Incoming() = default;
  Incoming(const ResourceLease& lease, std::size_t tiles_count,
           std::size_t values_count)
      : tile_allocation(lease),
        identity_allocation(lease),
        canonical_allocation(lease) {
    tiles.resize(tiles_count);
    tile_allocation.commit(tiles.capacity() * sizeof(tiles[0]));
    identities.resize(values_count);
    identity_allocation.commit(identities.capacity() * sizeof(identities[0]));
    canonical.resize(values_count);
    canonical_allocation.commit(canonical.capacity() * sizeof(canonical[0]));
  }
  Incoming(Incoming&&) noexcept = default;
  Incoming& operator=(Incoming&& other) noexcept {
    if (this != &other) {
      Incoming replaced(std::move(other));
      tile_allocation.swap(replaced.tile_allocation);
      identity_allocation.swap(replaced.identity_allocation);
      canonical_allocation.swap(replaced.canonical_allocation);
      tiles.swap(replaced.tiles);
      identities.swap(replaced.identities);
      canonical.swap(replaced.canonical);
    }
    return *this;
  }
  core_internal::MetadataAllocation tile_allocation, identity_allocation,
      canonical_allocation;
  std::vector<TileOwner> tiles;
  std::vector<std::shared_ptr<ValueIdentity>> identities;
  // Strong canonical owners survive only the boundary/admission handoff.
  std::vector<ResultRef> canonical;
};
Result<std::shared_ptr<ValueIdentity>> make_identity(
    const ResourceBudget& budget, Id original,
    const ResultRef* external = nullptr) {
  using Answer = Result<std::shared_ptr<ValueIdentity>>;
  auto lease = budget.reserve(ResourceCapacity::host(
      sizeof(ValueIdentity) + 64, sizeof(ValueIdentity) + 64));
  if (!lease.ok())
    return Answer(lease.status());
  auto state = std::make_shared<ValueIdentity>();
  state->metadata = lease.take_value();
  core_internal::ResourceMetadataAccess::commit(state->metadata,
                                                sizeof(ValueIdentity));
  state->original = original;
  if (external) {
    state->object = external->object_id();
    state->canonical = external->weak();
  } else {
    auto reserved = data_internal::ResultIdentity::reserve();
    if (!reserved.ok())
      return Answer(reserved.status());
    state->reservation = reserved.take_value();
    state->object = state->reservation.id();
  }
  return Answer(std::move(state));
}
bool complete_identity(const physical::Program& program,
                       const ValueIdentity& identity, Id value,
                       const TileRequest* selection = nullptr) {
  if (!identity.canonical_allowed)
    return false;
  const auto& original = program.values[identity.original];
  const auto& current = program.values[value];
  if (original.tiles.size() != current.tiles.size())
    return false;
  for (std::size_t i = 0; i < original.tiles.size(); ++i) {
    const auto& a = program.tiles[original.tiles[i]];
    const auto& b = program.tiles[current.tiles[i]];
    if (a.member != b.member || a.tile_index != b.tile_index)
      return false;
    if (selection) {
      auto member = selection->members.find(b.member);
      if (member == selection->members.end() ||
          !member->second.contains(b.tile_index))
        return false;
    }
  }
  return true;
}
}  // namespace
struct ContextState {
  static ResourceLease reserve(const ResourceBudget& budget,
                               std::uint32_t count,
                               const ExecutionContextConfig& config) {
    auto bytes =
        sizeof(ContextState) +
        CpuWorkers::metadata_bytes(count, config.maximum_queued_tasks) +
        std::uint64_t{config.maximum_runs} * sizeof(std::weak_ptr<RunState>);
    auto capacity = ResourceCapacity::host(bytes, bytes);
    // GPU submissions have no context FIFO; their completion slots are
    // reserved in the owning Run frame.
    capacity[ResourceKind::Queue] = config.maximum_queued_tasks;
    auto lease = budget.reserve(capacity);
    if (!lease.ok())
      throw std::invalid_argument("context resources exceed configured limits");
    auto admitted = lease.take_value();
    // The enclosing heap object exists before its constructor enters here.
    // Member-construction failure releases this lease after constructed
    // storage.
    core_internal::ResourceMetadataAccess::commit(admitted,
                                                  sizeof(ContextState));
    return admitted;
  }
  ContextState(std::shared_ptr<OperationRegistry> operations,
               std::uint32_t count, ExecutionContextConfig config,
               ResourceLimits limits)
      : registry(std::move(operations)),
        budget(std::move(limits)),
        metadata(reserve(budget, count, config)),
        workers(count, config.maximum_queued_tasks, metadata),
        cpu_count(count),
        gpu_count(config.maximum_gpu_submissions),
        gpu_available(gpu_count),
        maximum_runs(config.maximum_runs) {
    runs.reserve(maximum_runs);
    core_internal::ResourceMetadataAccess::commit(
        metadata, runs.capacity() * sizeof(runs[0]));
  }
  std::shared_ptr<OperationRegistry> registry;
  ResourceBudget budget;
  std::shared_ptr<gpu_internal::Device> device;
  ResourceLease metadata;
  CpuWorkers workers;
  FairAdmission cpu_admission, gpu_admission;
  std::uint32_t cpu_count, gpu_count;
  std::atomic<std::uint32_t> gpu_available;
  std::uint32_t maximum_runs;
  std::mutex mutex;
  bool closed = false;
  std::uint64_t next_run = 1;
  std::vector<std::weak_ptr<RunState>> runs;
};
struct PreparedState {
  ResourceLease metadata;
  std::shared_ptr<ContextState> context;
  ExecutionPlan plan;
  std::optional<NativeAllocations> native;
  std::vector<PreparedKernel> kernels;
  std::vector<ps_kernel_layout_v8> layouts;
  std::vector<std::vector<Id>> reads;
  std::vector<std::vector<std::uint32_t>> output_ports;
  std::vector<std::uint64_t> block_work;
  std::size_t maximum_reads = 0, maximum_writes = 0, maximum_calls = 0;
  bool has_gpu = false;
};

struct RunState : std::enable_shared_from_this<RunState> {
  struct Frame;
  struct Work {
    RunState* run = nullptr;
    Frame* frame = nullptr;
    std::uint32_t lane = 0;
    Submission submission;
    ps_kernel_invocation_v8 invocation{};
    ps_kernel_status_v8 status{};
    ps_kernel_completion_v8 gpu_completion{};
    ExecutionTaskTiming timing;
    Status completion;
    std::vector<ps_kernel_const_view_v8> inputs;
    std::vector<std::uint64_t> input_objects;
    std::vector<ps_kernel_mutable_view_v8> outputs;
    std::array<ps_kernel_scratch_v8, 3> scratch{};
    bool busy = false, done = false, completion_fault = false,
         timing_reported = false;
  };
  struct Frame {
    explicit Frame(RunState& owner, Id block, Incoming incoming);
    ~Frame() noexcept;
    void release_readers(Id task);
    void update_head_wait(bool aborting = false);
    void abort(const Status& status);
    void begin_abort() noexcept;
    void finish_drain() noexcept;
    std::optional<std::chrono::steady_clock::time_point> head_started;
    void release_tile(Id tile);
    RunState& run;
    Id block;
    FairAdmission::Waiter cpu_waiter, gpu_waiter;
    core_internal::MetadataAllocation allocation;
    TaskSchedule schedule;
    core_internal::MetadataAllocation tile_allocation, identity_allocation;
    BlockMemory memory;
    std::vector<TileOwner> tiles;
    std::vector<std::shared_ptr<ValueIdentity>> identities;
    std::vector<ResultRef> canonical;
    std::vector<std::uint32_t> holds;
    std::vector<Work> work;
    ResourceVector<DeliveryRecord> exports;
    ResourceMap<ResultRef> final_results;
    std::size_t skipped = 0;
  };
  std::shared_ptr<const PreparedState> prepared;
  ExecutionOptions options;
  CancellationToken cancellation;
  CancellationSource stop;
  ResourceLease metadata;
  std::uint64_t frame_metadata = 0;
  Incoming initial;
  std::vector<std::shared_ptr<ValueIdentity>> input_identities;
  std::uint64_t id = 0, activation = 0, submission = 0, sequence = 0;
  std::uint64_t remaining_work = 0;
  const std::chrono::steady_clock::time_point epoch =
      std::chrono::steady_clock::now();
  std::uint32_t lanes = 1, gpu_lanes = 0;
  std::mutex mutex;
  std::condition_variable changed;
  bool terminal = false;
  Status abort_status, block_status;
  ExecutionResult result;
  std::shared_ptr<DiagnosticsState> diagnostics;
  std::thread delivery;
  std::thread::id delivery_id;
  std::vector<std::optional<DeliveryRecord>> records;
  std::size_t record_head = 0, record_count = 0, delivery_pending = 0;
  bool delivery_closed = false, delivery_stop = false, delivery_error = false;
  std::uint64_t delivery_failed_sequence = 0, delivery_failed_call = 0;
  std::atomic<std::uint64_t> observer_queue_wait{0}, observer_invoke{0},
      observer_deliveries{0};

  const physical::Program& program() const { return prepared->plan.program(); }
  std::uint64_t elapsed(std::chrono::steady_clock::time_point time =
                            std::chrono::steady_clock::now()) const noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time - epoch)
            .count());
  }
  bool cancel() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (terminal)
      return false;
    const bool changed_stop = stop.cancel();
    changed.notify_all();
    return changed_stop;
  }
  void observe(const Status& status) {
    if (priority(status) > priority(abort_status))
      copy_status(&abort_status, status);
  }
  ErrorCode poll() {
    const bool cancelled = cancellation.cancelled() || stop.token().cancelled();
    const bool current = prepared->plan.current();
    std::lock_guard<std::mutex> lock(mutex);
    if (delivery_error) {
      Status error{ErrorCode::OperationFailed,
                   {},
                   FailureReason::HostException};
      error.detail.origin = FailureOrigin::Observer;
      error.detail.scope = FailureScope::Run;
      error.detail.node_id = delivery_failed_call;
      error.detail.call_sequence = delivery_failed_sequence;
      observe(std::move(error));
      delivery_error = false;
      --delivery_pending;
    }
    if (cancelled)
      observe({ErrorCode::Cancelled, {}});
    else if (!current)
      observe({ErrorCode::Stale, {}});
    return abort_status.code;
  }
  bool charge(std::uint64_t amount) {
    if (amount > remaining_work) {
      observe(failure(ErrorCode::ResourceExhausted, "Run work limit"));
      return false;
    }
    remaining_work -= amount;
    auto status = prepared->context->budget.consume({amount});
    if (!status.ok())
      observe(status);
    return status.ok();
  }
  void publish_diagnostics() noexcept {
    std::lock_guard<std::mutex> lock(diagnostics->mutex);
    result.diagnostics.memory = diagnostics->memory.snapshot();
    result.diagnostics.metadata = diagnostics->metadata_observation->snapshot();
    result.diagnostics.observer_queue_wait_ns = observer_queue_wait.load();
    result.diagnostics.observer_invoke_ns = observer_invoke.load();
    result.diagnostics.observer_deliveries = observer_deliveries.load();
    diagnostics->snapshot = result.diagnostics;
  }
  void record_task(Work* item) noexcept;
  void record_issue(physical::Backend backend, bool rejected = false) noexcept;
  void record_call(const CallRecord& record) noexcept;
  void deliver();
  void enqueue(DeliveryRecord record);
  void settle_delivery();
  Result<ResultRef> value(const Frame& frame, Id value,
                          const TileRequest* selection = nullptr);
  const Status& execute_block(Frame* frame);
  void run() noexcept;
  static CpuWork acquire(void* pointer) noexcept;
  static void invoke(void* pointer) noexcept;
  static void complete(void* pointer) noexcept;
  static void complete_gpu(void* pointer, ps_kernel_completion_key_v8 key,
                           const ps_kernel_status_v8* status) noexcept;
  static void report_timing(void* pointer, ps_kernel_completion_key_v8 key,
                            const ps_kernel_device_timing_v8* timing) noexcept;
  static void bind(Work* item, Submission submission) noexcept;
  void submit_gpu(Work* item);
};

// NOLINTBEGIN(whitespace/indent_namespace)
RunState::Frame::Frame(RunState& owner, Id block_id, Incoming incoming)
    : run(owner),
      block(block_id),
      cpu_waiter(owner.prepared->context->cpu_admission),
      gpu_waiter(owner.prepared->context->gpu_admission),
      allocation(owner.metadata),
      schedule(owner.program(), block_id, owner.id, owner.activation,
               owner.submission, owner.sequence, owner.metadata),
      tile_allocation(std::move(incoming.tile_allocation)),
      identity_allocation(std::move(incoming.identity_allocation)),
      tiles(std::move(incoming.tiles)),
      identities(std::move(incoming.identities)) {
  // NOLINTEND
  const auto& p = run.program();
  canonical.resize(p.values.size());
  allocation.commit(canonical.capacity() * sizeof(canonical[0]));
  holds.resize(p.tiles.size());
  allocation.commit(holds.capacity() * sizeof(holds[0]));
  work.resize(owner.lanes + owner.gpu_lanes);
  allocation.commit(work.capacity() * sizeof(work[0]));
  for (Id vi = 0; vi < p.values.size(); ++vi) {
    const auto& value = p.values[vi];
    if (value.kind == physical::ValueKind::Input) {
      identities[vi] = run.input_identities[vi];
    } else if (value.kind == physical::ValueKind::Imported &&
               value.owner == block) {
      // Import preserves the input association seen by kernels, but its COPY
      // storage is exported as a distinct Result object.
      auto identity = make_identity(run.prepared->context->budget, vi);
      if (!identity.ok())
        throw identity.status();
      identities[vi] = identity.take_value();
      identities[vi]->object =
          run.input_identities[value.import_source]->object;
    } else if (value.kind == physical::ValueKind::Produced &&
               p.calls[value.owner].block == block) {
      auto identity = make_identity(run.prepared->context->budget, vi);
      if (!identity.ok())
        throw identity.status();
      identities[vi] = identity.take_value();
    }
  }
  ResourceAdmissionFailure failure;
  std::uint64_t allocations = 0;
  auto allocated = BlockMemory::allocate(
      p, block, run.lanes, run.prepared->context->budget, run.gpu_lanes,
      run.prepared->native ? &*run.prepared->native : nullptr, &failure,
      &allocations,
      std::shared_ptr<MemoryObservation>(run.diagnostics,
                                         &run.diagnostics->memory));
  run.result.diagnostics.allocation_count += allocations;
  if (!allocated.ok()) {
    run.result.diagnostics.last_block_admission_failure = BlockAdmissionFailure{
        block, run.activation, allocated.status().code, failure};
    throw allocated.status();
  }
  memory = allocated.take_value();
  core_internal::MetadataAllocation root_allocation(owner.metadata);
  std::vector<std::uint8_t> roots(p.tiles.size());
  root_allocation.commit(roots.capacity() * sizeof(roots[0]));
  auto keep = [&](Id value) {
    for (auto tile : p.values[value].tiles)
      roots[tile] = true;
  };
  const auto& b = p.blocks[block];
  if (b.condition != physical::none)
    keep(b.condition);
  for (const auto& edge : b.edges) {
    for (std::size_t argument = 0; argument < edge.arguments.size();
         ++argument) {
      const auto& source = p.values[edge.arguments[argument]];
      const auto& target = p.values[p.blocks[edge.target].arguments[argument]];
      std::size_t cursor = 0;
      for (auto tile : target.tiles) {
        const auto& to = p.tiles[tile];
        while (p.tiles[source.tiles[cursor]].member != to.member ||
               p.tiles[source.tiles[cursor]].tile_index != to.tile_index)
          ++cursor;
        roots[source.tiles[cursor]] = true;
      }
    }
    for (auto value : edge.live_values)
      keep(value);
  }
  if (b.end == physical::EndKind::Return) {
    for (const auto& named : run.prepared->plan.outputs()) {
      const auto& selection =
          run.prepared->plan.output_requests().at(named.first);
      for (auto tile : p.values[b.return_values[named.second]].tiles) {
        auto member = selection.members.find(p.tiles[tile].member);
        if (member != selection.members.end() &&
            member->second.contains(p.tiles[tile].tile_index))
          roots[tile] = true;
      }
    }
  }
  for (Id tile = 0; tile < roots.size(); ++tile)
    holds[tile] = roots[tile];
  for (auto task : b.tasks)
    for (auto tile : run.prepared->reads[task])
      ++holds[tile];
  for (auto call : b.calls)
    for (auto value : p.calls[call].returns)
      for (auto tile : p.values[value].tiles)
        ++holds[tile];
  for (Id tile = 0; tile < tiles.size(); ++tile)
    if (!holds[tile])
      tiles[tile] = {};
  for (std::uint32_t lane = 0; lane < work.size(); ++lane) {
    work[lane].run = &run;
    work[lane].frame = this;
    work[lane].lane = lane;
    work[lane].inputs.reserve(run.prepared->maximum_reads);
    allocation.commit(work[lane].inputs.capacity() *
                      sizeof(work[lane].inputs[0]));
    work[lane].input_objects.reserve(run.prepared->maximum_reads);
    allocation.commit(work[lane].input_objects.capacity() *
                      sizeof(work[lane].input_objects[0]));
    work[lane].outputs.reserve(run.prepared->maximum_writes);
    allocation.commit(work[lane].outputs.capacity() *
                      sizeof(work[lane].outputs[0]));
    work[lane].completion.message.reserve(PS_KERNEL_MESSAGE_CAPACITY_V8);
    static_assert(sizeof(std::string) < PS_KERNEL_MESSAGE_CAPACITY_V8);
    allocation.commit(work[lane].completion.message.capacity() + 1);
  }
  // These envelopes remain private while their backing is writable. Building
  // every directory and ownership wrapper here makes retirement
  // allocation-free.
  ResourceAllocationScope scope(run.prepared->context->budget);
  exports = ResourceVector<DeliveryRecord>(
      ResourceAllocator<DeliveryRecord>(run.prepared->context->budget));
  exports.resize(b.calls.size());
  for (auto& record : exports)
    record = DeliveryRecord(run.metadata);
  if (run.options.on_call_retired) {
    for (std::size_t ordinal = 0; ordinal < b.calls.size(); ++ordinal)
      for (auto vi : p.calls[b.calls[ordinal]].returns) {
        auto exported = run.value(*this, vi);
        if (!exported.ok())
          throw exported.status();
        exports[ordinal].record.values.emplace(vi, exported.take_value());
      }
  }
  final_results = make_resource_map<ResultRef>(run.prepared->context->budget);
  if (b.end == physical::EndKind::Return) {
    for (const auto& output : run.prepared->plan.outputs()) {
      auto returned =
          run.value(*this, b.return_values[output.second],
                    &run.prepared->plan.output_requests().at(output.first));
      if (!returned.ok())
        throw returned.status();
      final_results.emplace(output.first, returned.take_value());
    }
  }
  // A complete canonical object may cross an edge only while that edge keeps
  // all of its original tiles live. Partial aliases carry identity alone.
  for (const auto& edge : b.edges) {
    for (std::size_t i = 0; i < edge.arguments.size(); ++i) {
      const auto source = edge.arguments[i];
      const auto target = p.blocks[edge.target].arguments[i];
      if (identities[source] &&
          complete_identity(p, *identities[source], target))
        canonical[source] = identities[source]->canonical.lock();
    }
    for (auto source : edge.live_values)
      if (identities[source] &&
          complete_identity(p, *identities[source], source))
        canonical[source] = identities[source]->canonical.lock();
  }
  schedule.admitted();
}
void RunState::Frame::update_head_wait(bool aborting) {
  const auto now = std::chrono::steady_clock::now();
  auto& d = run.result.diagnostics;
  if (head_started) {
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now -
                                                             *head_started)
            .count());
    d.current_head_wait_ns = elapsed;
    if (aborting || schedule.aborted()) {
      d.unfinished_head_wait_ns += elapsed;
      head_started.reset();
    } else if (!schedule.head_waiting()) {
      d.retirement_head_wait_ns += elapsed;
      d.peak_retirement_head_wait_ns =
          std::max(d.peak_retirement_head_wait_ns, elapsed);
      head_started.reset();
    }
    if (!head_started)
      d.current_head_wait_ns = 0;
  }
  if (!head_started && !aborting && schedule.head_waiting())
    head_started = now;
}
RunState::Frame::~Frame() noexcept {
  run.prepared->context->cpu_admission.cancel(cpu_waiter);
  run.prepared->context->gpu_admission.cancel(gpu_waiter);
  update_head_wait(true);
  if (std::any_of(work.begin(), work.end(),
                  [](const Work& item) { return item.busy; })) {
    begin_abort();
    run.publish_diagnostics();
  }
  // Unwinding must not free borrowed views, scratch or payload before workers
  // publish their terminal events, even when coordinator allocation fails.
  std::unique_lock<std::mutex> lock(run.mutex);
  run.changed.wait(lock, [&] {
    return std::all_of(work.begin(), work.end(), [](const Work& item) {
      return !item.busy || item.done;
    });
  });
  for (auto& item : work) {
    if (!item.busy)
      continue;
    item.timing.consumed_ns = run.elapsed();
    // Completion strings were reserved before issue. This also preserves
    // terminal accounting when coordinator work unwinds exceptionally.
    assign_kernel_status(&item.completion, item.status);
    run.record_task(&item);
    item.busy = false;
    if (item.lane >= run.lanes)
      ++run.prepared->context->gpu_available;
  }
  finish_drain();
}
void RunState::Frame::begin_abort() noexcept {
  auto& timing = run.result.diagnostics.abort_drain;
  if (!timing) {
    std::uint32_t inflight = 0;
    for (const auto& item : work)
      inflight += item.busy;
    timing = ExecutionDrainTiming{block, run.activation, run.elapsed(),
                                  0,     inflight,       false};
  }
}
void RunState::Frame::abort(const Status& status) {
  run.prepared->context->cpu_admission.cancel(cpu_waiter);
  run.prepared->context->gpu_admission.cancel(gpu_waiter);
  begin_abort();
  run.observe(status);
  schedule.abort(status);
}
void RunState::Frame::finish_drain() noexcept {
  auto& timing = run.result.diagnostics.abort_drain;
  if (timing && !timing->finished) {
    timing->completed_ns = run.elapsed();
    timing->finished = true;
  }
}
void RunState::Frame::release_tile(Id tile) {
  if (!--holds[tile])
    tiles[tile] = {};
}
void RunState::Frame::release_readers(Id task) {
  for (auto tile : run.prepared->reads[task])
    release_tile(tile);
}
CpuWork RunState::acquire(void* pointer) noexcept {
  auto& item = *static_cast<Work*>(pointer);
  auto& frame = *item.frame;
  auto submission = frame.schedule.issue_next(physical::Backend::Cpu);
  if (!submission)
    return {};
  bind(&item, *submission);
  return {&item, invoke, complete};
}
void RunState::bind(Work* pointer, Submission submission) noexcept {
  auto& item = *pointer;
  auto& self = *item.run;
  auto& frame = *item.frame;
  item.submission = submission;
  const auto& p = self.program();
  const auto& task = p.tasks[submission.task];
  item.timing = {};
  item.timing.activation = submission.key.activation;
  item.timing.submission = submission.key.submission;
  item.timing.block = frame.block;
  item.timing.task = submission.task;
  item.timing.call = task.call;
  item.timing.source_call =
      task.call == physical::none ? 0 : p.calls[task.call].source_call;
  item.timing.backend = p.placements[task.placement].backend;
  item.timing.ready_ns = self.elapsed(frame.schedule.ready_at(submission.task));
  item.inputs.clear();
  item.input_objects.clear();
  for (const auto& read : task.reads) {
    const auto& tile = p.tiles[read.tile];
    const auto& owner = frame.tiles[read.tile];
    item.inputs.push_back(
        {owner.allocation(), owner.device ? owner.device->size() : tile.bytes,
         (owner.device ? owner.offset : 0) + p.layouts[read.layout].byte_offset,
         &self.prepared->layouts[read.layout],
         static_cast<std::uint32_t>(tile.domain), read.port, tile.member, 0,
         tile.tile_index});
    item.input_objects.push_back(frame.identities[tile.value]->object);
  }
  item.outputs.clear();
  for (std::size_t i = 0; i < task.writes.size(); ++i) {
    auto id = task.writes[i];
    const auto& tile = p.tiles[id];
    auto* allocation = tile.domain == physical::Domain::Device
                           ? frame.memory.device_tile(id)->handle()
                           : static_cast<void*>(frame.memory.tile(id).data());
    item.outputs.push_back({allocation, tile.bytes,
                            p.layouts[tile.layout].byte_offset,
                            &self.prepared->layouts[tile.layout],
                            static_cast<std::uint32_t>(tile.domain),
                            self.prepared->output_ports[submission.task][i],
                            tile.member, 0, tile.tile_index});
  }
  item.invocation = {};
  item.invocation.inputs = item.inputs.data();
  item.invocation.input_count = static_cast<std::uint32_t>(item.inputs.size());
  item.invocation.input_object_ids = item.input_objects.data();
  item.invocation.outputs = item.outputs.data();
  item.invocation.output_count =
      static_cast<std::uint32_t>(item.outputs.size());
  item.invocation.scratch = item.scratch.data();
  for (unsigned domain = 0; domain < 3; ++domain)
    if (task.scratch[domain])
      item.scratch[item.invocation.scratch_count++] = {
          domain == 1 ? frame.memory.device_scratch(item.lane)->handle()
                      : static_cast<void*>(
                            frame.memory
                                .scratch(item.lane,
                                         static_cast<physical::Domain>(domain))
                                .data()),
          task.scratch[domain], domain, 0};
  item.invocation.parameters = task.parameters.data();
  item.invocation.parameter_bytes = task.parameters.size();
  item.invocation.cancellation = {
      &self, [](const void* context) -> std::uint32_t {
        const auto& run = *static_cast<const RunState*>(context);
        return run.cancellation.cancelled() || run.stop.token().cancelled();
      }};
  if (task.kind != physical::TaskKind::Copy ||
      p.placements[task.placement].backend == physical::Backend::Gpu)
    self.prepared->kernels[submission.task].bind(&item.invocation);
  item.status = {};
  item.done = false;
  item.busy = true;
  item.completion_fault = false;
  item.timing_reported = false;
  item.gpu_completion = {&item,
                         {submission.key.run, submission.key.activation,
                          submission.key.submission},
                         complete_gpu,
                         self.options.trace_capacity ? report_timing : nullptr};
  item.timing.submit_ns = self.elapsed();
  const auto wait = frame.schedule.ready_wait(submission.task);
  item.timing.lane_wait_ns = wait.lane_ns;
  item.timing.context_wait_ns = wait.context_ns;
  item.timing.coordinator_delay_ns = item.timing.submit_ns -
                                     item.timing.ready_ns - wait.lane_ns -
                                     wait.context_ns;
}
void RunState::report_timing(
    void* pointer, ps_kernel_completion_key_v8 key,
    const ps_kernel_device_timing_v8* timing) noexcept {
  auto& item = *static_cast<Work*>(pointer);
  std::lock_guard<std::mutex> lock(item.run->mutex);
  const auto& expected = item.gpu_completion.key;
  if (!item.busy || item.done || item.timing_reported || !timing ||
      key.run != expected.run || key.activation != expected.activation ||
      key.submission != expected.submission) {
    item.completion_fault = true;
  } else {
    item.timing.device = *timing;
    item.timing_reported = true;
  }
}
void RunState::complete_gpu(void* pointer, ps_kernel_completion_key_v8 key,
                            const ps_kernel_status_v8* status) noexcept {
  auto& item = *static_cast<Work*>(pointer);
  auto* run = item.run;
  std::lock_guard<std::mutex> lock(run->mutex);
  const auto& expected = item.gpu_completion.key;
  if (!item.busy || item.done || key.run != expected.run ||
      key.activation != expected.activation ||
      key.submission != expected.submission) {
    // A foreign event cannot discharge the real submission's borrowing.
    item.completion_fault = true;
  } else {
    item.status = status ? *status
                         : kernel_failure(ErrorCode::Internal,
                                          "missing GPU completion status");
    item.timing.completion_ns = run->elapsed();
    item.done = true;
  }
  run->changed.notify_all();
}
void RunState::submit_gpu(Work* item) {
  auto& context = *prepared->context;
  auto& waiter = item->frame->gpu_waiter;
  if (!item->frame->schedule.has_ready(physical::Backend::Gpu)) {
    context.gpu_admission.cancel(waiter);
    return;
  }
  const bool acquired = context.gpu_admission.try_acquire(waiter, [&] {
    auto available = context.gpu_available.load();
    while (available && !context.gpu_available.compare_exchange_weak(
                            available, available - 1)) {
    }
    return available != 0;
  });
  if (!acquired)
    return;
  auto submission = item->frame->schedule.issue_next(physical::Backend::Gpu);
  if (!submission) {
    ++context.gpu_available;
    return;
  }
  bind(item, *submission);
  const auto accepted = prepared->kernels[submission->task].submit(
      item->invocation, item->gpu_completion);
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (accepted.accepted == 0) {
      // Rejection owns no asynchronous work, so it can settle locally.
      if (item->done || item->timing_reported ||
          accepted.status.code == PS_KERNEL_OK_V8 || accepted.reserved)
        item->status = kernel_failure(ErrorCode::Internal,
                                      "invalid GPU submission rejection");
      else
        item->status = accepted.status;
      item->timing.completion_ns = elapsed();
      item->done = true;
    } else if (accepted.accepted != 1 || accepted.reserved ||
               accepted.status.code != PS_KERNEL_OK_V8) {
      // Acceptance may have transferred borrowing even on protocol failure.
      // Abort only after the backend supplies its terminal completion.
      item->completion_fault = true;
    }
  }
  record_issue(physical::Backend::Gpu, accepted.accepted == 0);
}
void RunState::invoke(void* pointer) noexcept {
  auto& item = *static_cast<Work*>(pointer);
  item.timing.cpu_start_ns = item.run->elapsed();
  const auto& p = item.run->program();
  const auto& task = p.tasks[item.submission.task];
  if (task.kind != physical::TaskKind::Copy) {
    item.status = item.run->prepared->kernels[item.submission.task].invoke(
        item.invocation);
    return;
  }
  const auto& input = item.inputs[0];
  const auto& output = item.outputs[0];
  const auto width =
      TensorSpan::element_size(p.layouts[task.reads[0].layout].element_type);
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < input.layout->rank; ++axis)
    count *= input.layout->shape[axis];
  for (std::uint64_t sample = 0; sample < count; ++sample) {
    auto remainder = sample, offset = input.byte_offset;
    for (unsigned axis = input.layout->rank; axis-- > 0;) {
      auto coordinate = remainder % input.layout->shape[axis];
      remainder /= input.layout->shape[axis];
      auto stride = input.layout->strides[axis];
      if (stride < 0)
        offset -= coordinate * (static_cast<std::uint64_t>(-(stride + 1)) + 1);
      else
        offset += coordinate * static_cast<std::uint64_t>(stride);
    }
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) + sample * width,
                static_cast<const std::uint8_t*>(input.allocation) + offset,
                width);
  }
}
void RunState::complete(void* pointer) noexcept {
  auto& item = *static_cast<Work*>(pointer);
  auto* run = item.run;
  {
    std::lock_guard<std::mutex> lock(run->mutex);
    item.timing.completion_ns = run->elapsed();
    item.done = true;
    // The coordinator may release the last Run owner as soon as it observes
    // done. Finish every access to the Run before publishing this unlock.
    run->changed.notify_all();
  }
}

void RunState::record_issue(physical::Backend backend, bool rejected) noexcept {
  auto& d = result.diagnostics;
  auto& b = d.backends[static_cast<unsigned>(backend)];
  ++b.submitted;
  ++b.inflight;
  b.rejected += rejected;
  b.peak_inflight = std::max(b.peak_inflight, b.inflight);
  ++d.submitted_tasks;
  d.peak_active_tasks = std::max(
      d.peak_active_tasks, d.backends[0].inflight + d.backends[1].inflight);
}
void RunState::record_task(Work* item) noexcept {
  auto& timing = item->timing;
  timing.status = item->completion.code;
  const auto& task = program().tasks[timing.task];
  if (timing.status == ErrorCode::Ok && task.kind == physical::TaskKind::Copy) {
    const auto& layout = program().layouts[task.reads[0].layout];
    std::uint64_t bytes = TensorSpan::element_size(layout.element_type);
    for (unsigned i = 0; i < layout.rank; ++i)
      bytes *= layout.shape[i];
    timing.copy_bytes = bytes;
  }
  auto& d = result.diagnostics;
  auto& b = d.backends[static_cast<unsigned>(timing.backend)];
  --b.inflight;
  ++b.completed;
  b.inflight_ns += timing.consumed_ns - timing.submit_ns;
  ++d.completed_tasks;
  d.ready_wait_ns += timing.submit_ns - timing.ready_ns;
  d.lane_wait_ns += timing.lane_wait_ns;
  d.context_wait_ns += timing.context_wait_ns;
  d.coordinator_delay_ns += timing.coordinator_delay_ns;
  d.completion_delay_ns += timing.consumed_ns - timing.completion_ns;
  d.copy_bytes += timing.copy_bytes;
  if (timing.backend == physical::Backend::Cpu) {
    d.cpu_queue_wait_ns += timing.cpu_start_ns - timing.submit_ns;
    d.cpu_invoke_ns += timing.completion_ns - timing.cpu_start_ns;
  }
  if (!options.trace_capacity)
    return;
  std::lock_guard<std::mutex> lock(diagnostics->mutex);
  if (diagnostics->task_count < diagnostics->tasks.size()) {
    diagnostics->tasks[diagnostics->task_count++] = timing;
    ++d.task_timing_count;
  } else {
    ++d.dropped_task_timings;
  }
}
void RunState::record_call(const CallRecord& record) noexcept {
  if (!options.trace_capacity)
    return;
  const ExecutionCallTiming timing{record.activation,  record.sequence,
                                   record.source_call, elapsed(),
                                   record.call,        record.outcome};
  std::lock_guard<std::mutex> lock(diagnostics->mutex);
  if (diagnostics->call_count < diagnostics->calls.size()) {
    diagnostics->calls[diagnostics->call_count++] = timing;
    ++result.diagnostics.call_timing_count;
  } else {
    ++result.diagnostics.dropped_call_timings;
  }
}

void RunState::deliver() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    delivery_id = std::this_thread::get_id();
  }
  for (;;) {
    DeliveryRecord owned;
    auto& record = owned.record;
    {
      std::unique_lock<std::mutex> lock(mutex);
      changed.wait(lock, [&] {
        return record_count || delivery_stop || delivery_closed;
      });
      if (delivery_closed || (!record_count && delivery_stop))
        return;
      owned = std::move(*records[record_head]);
      records[record_head].reset();
      record_head = (record_head + 1) % records.size();
      --record_count;
    }
    bool failed = false;
    const auto started = elapsed();
    try {
      core_internal::CallbackEnvironment environment;
      options.on_call_retired(record);
    } catch (...) {
      failed = true;
    }
    observer_queue_wait.fetch_add(started - owned.queued_ns);
    observer_invoke.fetch_add(elapsed() - started);
    // Release the lane's owners before reporting settlement.
    const auto delivered_sequence = record.sequence;
    const auto delivered_call = record.source_call;
    owned = {};
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (failed) {
        delivery_closed = true;
        delivery_error = true;
        delivery_failed_sequence = delivered_sequence;
        delivery_failed_call = delivered_call;
        for (auto& pending : records)
          pending.reset();
        delivery_pending -= record_count;
        record_count = 0;
      } else {
        --delivery_pending;
      }
      observer_deliveries.fetch_add(1);
    }
    changed.notify_all();
    if (failed)
      return;
  }
}
void RunState::enqueue(DeliveryRecord record) {
  if (!options.on_call_retired)
    return;
  std::lock_guard<std::mutex> lock(mutex);
  if (delivery_closed)
    return;
  if (record_count == records.size())
    throw failure(ErrorCode::Internal, "retirement delivery capacity violated");
  record.queued_ns = elapsed();
  records[(record_head + record_count) % records.size()] = std::move(record);
  ++record_count;
  ++delivery_pending;
  changed.notify_all();
}
void RunState::settle_delivery() {
  for (;;) {
    poll();
    std::unique_lock<std::mutex> lock(mutex);
    if (!delivery_pending)
      return;
    changed.wait_for(lock, std::chrono::milliseconds(2));
  }
}
Result<ResultRef> RunState::value(const Frame& frame, Id vi,
                                  const TileRequest* selection) {
  const auto& p = program();
  const auto& v = p.values[vi];
  const auto& identity = frame.identities[vi];
  if (!identity)
    return Result<ResultRef>(
        failure(ErrorCode::Internal, "missing dynamic value identity"));
  const bool canonical = complete_identity(p, *identity, vi, selection);
  if (canonical) {
    auto existing = identity->canonical.lock();
    if (existing.valid())
      return Result<ResultRef>(std::move(existing));
    if (!identity->reservation.id())
      return Result<ResultRef>(
          failure(ErrorCode::Internal, "expired canonical Result identity"));
  }
  auto made = canonical ? std::move(identity->reservation)
                              .materialize(prepared->context->budget,
                                           *p.schemas[v.schema], "physical-run",
                                           {}, p.tile_extent)
                        : ResultBuilder::start(
                              prepared->context->budget, *p.schemas[v.schema],
                              "physical-run", {}, p.tile_extent);
  if (!made.ok())
    return Result<ResultRef>(made.status());
  auto builder = made.take_value();
  for (auto id : v.tiles) {
    const auto& tile = p.tiles[id];
    if (selection) {
      auto member = selection->members.find(tile.member);
      if (member == selection->members.end() ||
          !member->second.contains(tile.tile_index))
        continue;
    }
    auto owner = frame.tiles[id];
    if (tile.domain == physical::Domain::Device && !owner.device)
      owner.device = frame.memory.unpublished_device(id);
    if (tile.domain != physical::Domain::Device && !owner.storage)
      owner.storage = frame.memory.unpublished(id);
    if (!owner.storage && !owner.device)
      return Result<ResultRef>(
          failure(ErrorCode::Internal, "missing completed tile"));
    const auto& layout = p.layouts[tile.layout];
    TileView view(metadata, layout, true, owner.offset);
    auto published =
        owner.device
            ? data_internal::ResultDeviceAccess::publish(
                  builder, tile.member, view.region, view.layout, owner.device)
            : builder.publish_tensor(tile.member, view.region, view.layout,
                                     owner.storage);
    if (!published.ok())
      return Result<ResultRef>(published);
  }
  auto sealed = builder.seal();
  if (sealed.ok() && canonical)
    identity->canonical = sealed.value().weak();
  return sealed;
}
const Status& RunState::execute_block(Frame* frame) {
  const auto& p = program();
  frame->update_head_wait();
  for (;;) {
    frame->schedule.waiting(physical::Backend::Cpu, AdmissionWait::Coordinator);
    frame->schedule.waiting(physical::Backend::Gpu, AdmissionWait::Coordinator);
    auto stopped = poll();
    if (stopped != ErrorCode::Ok)
      frame->abort(abort_status);
    for (auto& item : frame->work) {
      bool done, fault;
      {
        std::lock_guard<std::mutex> lock(mutex);
        done = item.busy && item.done;
        fault = item.busy && item.completion_fault;
      }
      if (fault)
        frame->abort(
            Status{ErrorCode::Internal, {}, FailureReason::MalformedEnvelope});
      if (!done)
        continue;
      item.timing.consumed_ns = elapsed();
      assign_kernel_status(&item.completion, item.status);
      auto& status = item.completion;
      const auto task = item.submission.task;
      if (!status.ok()) {
        status.detail.origin = FailureOrigin::Backend;
        status.detail.scope = FailureScope::Run;
        auto ci = p.tasks[task].call;
        status.detail.node_id =
            ci == physical::none ? 0 : p.calls[ci].source_call;
        status.detail.task_id = task;
        if (p.placements[p.tasks[task].placement].backend ==
                physical::Backend::Gpu &&
            status.code == ErrorCode::BackendUnavailable)
          frame->abort(status);
      }
      if (status.ok()) {
        for (auto id : p.tasks[task].writes) {
          if (frame->holds[id]) {
            if (p.tiles[id].domain == physical::Domain::Device)
              frame->tiles[id] = {frame->memory.publish_device(id), 0};
            else
              frame->tiles[id] = {frame->memory.publish(id), 0};
          } else {
            frame->memory.discard(id);
          }
        }
      } else {
        for (auto id : p.tasks[task].writes)
          frame->memory.discard(id);
      }
      frame->release_readers(task);
      frame->schedule.complete(item.submission.key, status);
      record_task(&item);
      item.busy = false;
      if (item.lane >= lanes)
        ++prepared->context->gpu_available;
    }
    while (frame->skipped < frame->schedule.skipped().size()) {
      auto task = frame->schedule.skipped()[frame->skipped++];
      frame->release_readers(task);
      for (auto id : p.tasks[task].writes)
        frame->memory.discard(id);
    }
    frame->update_head_wait();
    while (auto retired = frame->schedule.retire_next()) {
      frame->update_head_wait();
      DeliveryRecord owned =
          std::move(frame->exports[p.calls[retired->call].ordinal]);
      auto& record = owned.record;
      record.sequence = retired->sequence;
      record.activation = retired->activation;
      record.source_call = retired->source_call;
      record.call = retired->call;
      record.failed_task = retired->failed_task;
      record.outcome =
          retired->state == CallState::Succeeded ? CallOutcome::Succeeded
          : retired->state == CallState::Failed  ? CallOutcome::Failed
                                                 : CallOutcome::Elided;
      if (record.outcome == CallOutcome::Failed)
        copy_status(&record.status,
                    frame->schedule.failure(record.failed_task));
      if (record.outcome != CallOutcome::Succeeded)
        record.values.clear();
      for (auto vi : p.calls[record.call].returns)
        for (auto id : p.values[vi].tiles)
          frame->release_tile(id);
      record_call(record);
      enqueue(std::move(owned));
      ++result.diagnostics.retired_calls;
    }
    if (frame->schedule.drained()) {
      prepared->context->cpu_admission.cancel(frame->cpu_waiter);
      prepared->context->gpu_admission.cancel(frame->gpu_waiter);
      frame->finish_drain();
      submission = frame->schedule.next_submission();
      sequence = frame->schedule.next_sequence();
      publish_diagnostics();
      frame->schedule.copy_settled_status(&block_status);
      return block_status;
    }
    if (poll() != ErrorCode::Ok)
      frame->abort(abort_status);
    for (auto& item : frame->work) {
      if (item.busy)
        continue;
      if (item.lane >= lanes) {
        submit_gpu(&item);
        continue;
      }
      auto& context = *prepared->context;
      if (!frame->schedule.has_ready(physical::Backend::Cpu)) {
        context.cpu_admission.cancel(frame->cpu_waiter);
        continue;
      }
      auto status = CpuSubmission::Busy;
      context.cpu_admission.try_acquire(frame->cpu_waiter, [&] {
        status = context.workers.submit_ready(&item, acquire);
        return status == CpuSubmission::Accepted;
      });
      if (status == CpuSubmission::Accepted) {
        record_issue(physical::Backend::Cpu);
      } else if (status == CpuSubmission::Closed) {
        frame->abort(failure(ErrorCode::Cancelled, "context stopped"));
      }
    }
    frame->update_head_wait();
    for (unsigned backend = 0; backend < 2; ++backend) {
      const auto kind = static_cast<physical::Backend>(backend);
      const bool ready = frame->schedule.has_ready(kind);
      bool free_lane = false;
      const auto begin = backend ? lanes : 0;
      const auto end = backend ? lanes + gpu_lanes : lanes;
      for (auto lane = begin; lane < end; ++lane)
        free_lane |= !frame->work[lane].busy;
      frame->schedule.waiting(kind, !ready      ? AdmissionWait::Coordinator
                                    : free_lane ? AdmissionWait::Context
                                                : AdmissionWait::Lane);
    }
    if (frame->schedule.drained())
      continue;
    if (!frame->schedule.inflight() &&
        !frame->schedule.has_ready(physical::Backend::Cpu) &&
        !frame->schedule.has_ready(physical::Backend::Gpu)) {
      frame->abort(
          failure(ErrorCode::Internal, "admitted block cannot progress"));
      continue;
    }
    publish_diagnostics();
    std::unique_lock<std::mutex> lock(mutex);
    const auto event = [&] {
      if (!frame->schedule.aborted() &&
          (delivery_error || cancellation.cancelled() ||
           stop.token().cancelled()))
        return true;
      return std::any_of(
          frame->work.begin(), frame->work.end(), [&](const auto& item) {
            return item.busy && (item.done || (!frame->schedule.aborted() &&
                                               item.completion_fault));
          });
    };
    if (event())
      continue;
    ++result.diagnostics.coordinator_waits;
    changed.wait_for(lock, std::chrono::milliseconds(2), event);
    ++result.diagnostics.coordinator_wakeups;
  }
}
void RunState::run() noexcept {
  core_internal::ResourceMetadataScope observation(
      prepared->context->budget, diagnostics->metadata_observation);
  try {
    if (options.on_call_retired)
      delivery = std::thread([this] { deliver(); });
    auto incoming = std::move(initial);
    auto block = program().entry;
    for (;;) {
      if (poll() != ErrorCode::Ok)
        break;
      if (activation == options.maximum_block_activations ||
          activation == UINT64_MAX - 1) {
        observe(failure(ErrorCode::ResourceExhausted, "Run activation limit"));
        break;
      }
      ++activation;
      ++result.diagnostics.activations;
      const auto& b = program().blocks[block];
      if (!charge(prepared->block_work[block]))
        break;
      auto frame = [&]() {
        ResourceAdmissionFailure capacity;
        bool rejected = false;
        core_internal::ResourceAdmissionScope admission(
            prepared->context->budget, &capacity, &rejected);
        try {
          return Frame(*this, block, std::move(incoming));
        } catch (...) {
          ErrorCode code = ErrorCode::OperationFailed;
          try {
            throw;
          } catch (const Status& status) {
            code = status.code;
          } catch (const std::bad_alloc&) {
            code = ErrorCode::ResourceExhausted;
          } catch (...) {
          }
          ++result.diagnostics.block_admission_failures;
          if (!result.diagnostics.last_block_admission_failure ||
              result.diagnostics.last_block_admission_failure->activation !=
                  activation)
            result.diagnostics.last_block_admission_failure =
                BlockAdmissionFailure{block, activation, code, {}};
          if (rejected)
            result.diagnostics.last_block_admission_failure->capacity =
                capacity;
          throw;
        }
      }();
      const auto& status = execute_block(&frame);
      if (frame.schedule.aborted() && !status.ok())
        observe(status);
      // A task failure is the terminal candidate only after delivery settles.
      // Observer/context/device aborts outrank it even when the failed call's
      // own retirement callback is the callback that throws.
      settle_delivery();
      if (poll() != ErrorCode::Ok)
        break;
      if (!status.ok()) {
        observe(status);
        break;
      }
      if (b.end == physical::EndKind::Return) {
        result.results = std::move(frame.final_results);
        break;
      }
      unsigned choice = 0;
      if (b.end == physical::EndKind::Branch) {
        auto tile = program().values[b.condition].tiles[0];
        choice =
            frame.tiles[tile].data()[program()
                                         .layouts[program().tiles[tile].layout]
                                         .byte_offset]
                ? 0
                : 1;
      }
      const auto& edge = b.edges[choice];
      incoming =
          Incoming(metadata, program().tiles.size(), program().values.size());
      for (auto vi : edge.live_values) {
        incoming.identities[vi] = frame.identities[vi];
        incoming.canonical[vi] = frame.canonical[vi];
        for (auto tile : program().values[vi].tiles)
          incoming.tiles[tile] = frame.tiles[tile];
      }
      for (std::size_t i = 0; i < edge.arguments.size(); ++i) {
        const auto& source = program().values[edge.arguments[i]];
        const auto target_id = program().blocks[edge.target].arguments[i];
        const auto& target = program().values[target_id];
        incoming.identities[target_id] = frame.identities[edge.arguments[i]];
        if (complete_identity(program(), *incoming.identities[target_id],
                              target_id))
          incoming.canonical[target_id] = frame.canonical[edge.arguments[i]];
        std::size_t cursor = 0;
        for (auto tile : target.tiles) {
          const auto& to = program().tiles[tile];
          while (program().tiles[source.tiles[cursor]].member != to.member ||
                 program().tiles[source.tiles[cursor]].tile_index !=
                     to.tile_index)
            ++cursor;
          incoming.tiles[tile] = frame.tiles[source.tiles[cursor]];
        }
      }
      block = edge.target;
    }
  } catch (const Status& status) {
    observe(status);
  } catch (const std::bad_alloc&) {
    observe({ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    observe({ErrorCode::OperationFailed, {}});
  }
  // The frame destructor has drained all accepted invocations before here.
  settle_delivery();
  {
    std::lock_guard<std::mutex> lock(mutex);
    delivery_stop = true;
    changed.notify_all();
  }
  if (delivery.joinable())
    delivery.join();
  initial = {};
  input_identities.clear();
  records.clear();
  options.on_call_retired = {};
  if (frame_metadata) {
    metadata.shrink(ResourceCapacity::host(frame_metadata, frame_metadata));
    frame_metadata = 0;
  }
  poll();
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (cancellation.cancelled() || stop.token().cancelled())
      observe({ErrorCode::Cancelled, {}});
    else if (!prepared->plan.current())
      observe({ErrorCode::Stale, {}});
    if (!abort_status.ok())
      result.results.clear();
    publish_diagnostics();
    terminal = true;
  }
  changed.notify_all();
}
}  // namespace ps::execution_internal

namespace ps {
namespace {
using execution_internal::ContextState;
using execution_internal::PreparedState;
using execution_internal::RunState;
Status stale() {
  return {ErrorCode::Stale, "invalid, stale or foreign plan"};
}
Result<execution_internal::Incoming> bind_inputs(
    const PreparedState& prepared, const ExecutionBindings& bindings,
    const ResourceLease& metadata) {
  using Answer = Result<execution_internal::Incoming>;
  const auto& plan = prepared.plan;
  const auto& p = plan.program();
  if (bindings.inputs.size() != plan.input_declarations().size())
    return Answer({ErrorCode::InvalidArgument, "binding count mismatch"});
  ResourceVector<const ExecutionBinding*> named{
      ResourceAllocator<const ExecutionBinding*>(prepared.context->budget)};
  named.reserve(bindings.inputs.size());
  for (const auto& input : bindings.inputs) {
    if (!input.result.valid())
      return Answer({ErrorCode::InvalidArgument, "invalid binding"});
    named.push_back(&input);
  }
  std::sort(named.begin(), named.end(),
            [](const auto* a, const auto* b) { return a->name < b->name; });
  if (std::adjacent_find(named.begin(), named.end(),
                         [](const auto* a, const auto* b) {
                           return a->name == b->name;
                         }) != named.end())
    return Answer({ErrorCode::InvalidArgument, "duplicate binding"});
  execution_internal::Incoming result(metadata, p.tiles.size(),
                                      p.values.size());
  ResourceVector<const void*> allocations{
      ResourceAllocator<const void*>(prepared.context->budget)};
  for (std::size_t input = 0; input < plan.input_declarations().size();
       ++input) {
    const auto& declaration = plan.input_declarations()[input];
    auto found =
        std::lower_bound(named.begin(), named.end(), declaration.name,
                         [](const auto* binding, const std::string& name) {
                           return binding->name < name;
                         });
    if (found == named.end() || (*found)->name != declaration.name)
      return Answer({ErrorCode::InvalidArgument, "missing input binding"});
    if (!(*found)->result.schema().same_schema(*declaration.result_schema))
      return Answer({ErrorCode::TypeMismatch, "input schema mismatch"});
    auto snapshot = (*found)->result.capture();
    if (!snapshot.ok())
      return Answer(snapshot.status());
    auto descriptor = snapshot.value().descriptor();
    if (!descriptor.ok())
      return Answer(descriptor.status());
    const auto value_id = p.inputs[input];
    auto made_identity = execution_internal::make_identity(
        prepared.context->budget, value_id, &snapshot.value());
    if (!made_identity.ok())
      return Answer(made_identity.status());
    auto identity = made_identity.take_value();
    std::array<std::uint64_t, 16> counts{};
    for (auto id : p.values[p.inputs[input]].tiles) {
      const auto& tile = p.tiles[id];
      const auto& layout = p.layouts[tile.layout];
      execution_internal::TileView tile_view(metadata, layout, false);
      if (tile.domain == physical::Domain::Device) {
        auto view = data_internal::ResultDeviceAccess::acquire(
            snapshot.value(), descriptor.value(), tile.member,
            tile_view.region);
        if (!view.ok())
          return Answer(view.status());
        if (!prepared.native ||
            !prepared.native->device->owns(*view.value().storage()))
          return Answer({ErrorCode::TypeMismatch,
                         "Device binding belongs to a different device"});
        for (unsigned axis = 0; axis < layout.rank; ++axis)
          if (layout.shape[axis] > 1 &&
              view.value().layout().byte_strides[axis] != layout.strides[axis])
            return Answer({ErrorCode::TypeMismatch,
                           "Device input layout requires an explicit COPY"});
        auto offset =
            view.value().byte_address(layout.origin.data(), layout.rank);
        if (!offset.ok())
          return Answer(offset.status());
        if (offset.value() < layout.byte_offset)
          return Answer({ErrorCode::TypeMismatch,
                         "Device origin cannot bind compiled view"});
        const auto base = offset.value() - layout.byte_offset;
        const auto size = view.value().storage()->size();
        if (base > size || tile.bytes > size - base || base % tile.alignment)
          return Answer(
              {ErrorCode::TypeMismatch, "Device allocation contract mismatch"});
        auto owner = data_internal::ResultDeviceAccess::reference(
            prepared.context->budget, view.value().storage());
        if (!owner.ok())
          return Answer(owner.status());
        if (p.values[value_id].input_layout ==
            physical::InputLayout::DirectTiles)
          allocations.push_back(view.value().storage().get());
        result.tiles[id] = {owner.take_value(), base};
      } else {
        auto window = snapshot.value().acquire_tensor(
            descriptor.value(), tile.member, tile_view.region);
        if (!window.ok())
          return Answer(window.status());
        auto view = window.value().affine_view();
        if (!view.ok())
          return Answer(view.status());
        if (tile.domain == physical::Domain::Shared &&
            (!prepared.native ||
             !prepared.native->device->owns(*view.value().storage())))
          return Answer({ErrorCode::TypeMismatch,
                         "Shared binding belongs to a different device"});
        for (unsigned axis = 0; axis < layout.rank; ++axis)
          if (layout.shape[axis] > 1 &&
              view.value().layout().byte_strides[axis] != layout.strides[axis])
            return Answer({ErrorCode::TypeMismatch,
                           "input layout requires an explicit COPY"});
        auto offset =
            view.value().byte_address(layout.origin.data(), layout.rank);
        if (!offset.ok())
          return Answer(offset.status());
        if (offset.value() < layout.byte_offset)
          return Answer({ErrorCode::TypeMismatch,
                         "input origin cannot bind compiled view"});
        auto base = offset.value() - layout.byte_offset;
        const auto bytes = view.value().storage()->bytes();
        if (base > bytes.size() || tile.bytes > bytes.size() - base ||
            reinterpret_cast<std::uintptr_t>(bytes.data() + base) %
                tile.alignment)
          return Answer(
              {ErrorCode::TypeMismatch, "input allocation contract mismatch"});
        auto owner = prepared.context->budget.reference(view.value().storage());
        if (!owner.ok())
          return Answer(owner.status());
        if (p.values[p.inputs[input]].input_layout ==
            physical::InputLayout::DirectTiles)
          allocations.push_back(view.value().storage().get());
        result.tiles[id] = {owner.take_value(), base};
      }
      auto samples = tile_view.region.element_count();
      if (!samples.ok() ||
          !core_internal::checked_add(counts[tile.member], samples.value(),
                                      &counts[tile.member]))
        identity->canonical_allowed = false;
    }
    // Every selected tile was proved to be inside the captured coverage.
    // Disjoint equal cardinality therefore proves exact full coverage without
    // enumerating or retaining the source's potentially huge tile directory.
    for (unsigned member = 0; member < descriptor.value().tensor_count();
         ++member) {
      auto actual = descriptor.value().tensor_coverage(member).element_count();
      if (!actual.ok() || counts[member] != actual.value())
        identity->canonical_allowed = false;
    }
    if (identity->canonical_allowed)
      result.canonical[value_id] = snapshot.value();
    result.identities[value_id] = std::move(identity);
  }
  std::sort(allocations.begin(), allocations.end(), std::less<const void*>{});
  if (std::adjacent_find(allocations.begin(), allocations.end()) !=
      allocations.end())
    return Answer({ErrorCode::TypeMismatch,
                   "shared input backing requires an explicit COPY"});
  return Answer(std::move(result));
}
}  // namespace

ExecutionContext::ExecutionContext(
    std::shared_ptr<OperationRegistry> operations,
    ExecutionContextConfig config) {
  if (!operations || !operations->frozen() || !config.maximum_queued_tasks ||
      !config.maximum_runs || config.maximum_runs > 65536 ||
      config.cpu_workers > 4096 || config.maximum_gpu_submissions > 4096 ||
      !config.maximum_live_bytes)
    throw std::invalid_argument("invalid execution context configuration");
  auto count = config.cpu_workers;
  if (!count)
    count = std::max(1U, std::min(32U, std::thread::hardware_concurrency()));
  auto limits = config.managed_resources.value_or(ResourceLimits{});
  limits.capacity[ResourceKind::Payload] = std::min(
      limits.capacity[ResourceKind::Payload], config.maximum_live_bytes);
  state_ = std::make_shared<ContextState>(std::move(operations), count, config,
                                          std::move(limits));
}
ExecutionContext::~ExecutionContext() noexcept {
  std::vector<std::weak_ptr<RunState>> runs;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
    runs.swap(state_->runs);
  }
  for (const auto& weak : runs)
    if (auto run = weak.lock())
      run->cancel();
  for (const auto& weak : runs)
    if (auto run = weak.lock()) {
      std::unique_lock<std::mutex> lock(run->mutex);
      run->changed.wait(lock, [&] { return run->terminal; });
    }
  state_->workers.shutdown();
  const auto directory_bytes = runs.capacity() * sizeof(runs[0]);
  std::vector<std::weak_ptr<RunState>>().swap(runs);
  core_internal::ResourceMetadataAccess::withdraw(state_->metadata,
                                                  directory_bytes);
}
Result<PreparedPlan> ExecutionContext::prepare(
    const ExecutionPlan& plan) const {
  using Answer = Result<PreparedPlan>;
  if (!plan.current() ||
      plan.data().operation_registry_.lock() != state_->registry)
    return Answer(stale());
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed)
      return Answer({ErrorCode::Cancelled, "context stopped"});
  }
  try {
    const auto& p = plan.program();
    std::uint64_t bytes = sizeof(PreparedState) + 64;
    auto count = [&](std::size_t size, std::size_t width) {
      if (!core_internal::checked_multiply_add(size, width, bytes, &bytes))
        throw std::bad_alloc();
    };
    count(p.layouts.size(), sizeof(ps_kernel_layout_v8));
    count(p.tasks.size(), sizeof(PreparedKernel) +
                              sizeof(std::vector<physical::Id>) +
                              sizeof(std::vector<std::uint32_t>));
    count(p.blocks.size(), sizeof(std::uint64_t));
    for (const auto& task : p.tasks) {
      count(task.reads.size(), sizeof(physical::Id));
      count(task.writes.size(), sizeof(std::uint32_t));
    }
    auto metadata =
        state_->budget.reserve(ResourceCapacity::host(bytes, bytes));
    if (!metadata.ok())
      return Answer(metadata.status());
    auto prepared = std::make_shared<PreparedState>();
    prepared->metadata = metadata.take_value();
    core_internal::ResourceMetadataAccess::commit(prepared->metadata,
                                                  sizeof(PreparedState));
    prepared->context = state_;
    prepared->plan = plan;
    bool needs_native = false;
    for (const auto& tile : p.tiles)
      needs_native |= tile.domain != physical::Domain::Host;
    for (const auto& task : p.tasks)
      needs_native |=
          task.scratch[1] != 0 || task.scratch[2] != 0 ||
          p.placements[task.placement].domain != physical::Domain::Host;
    if (needs_native) {
      std::shared_ptr<gpu_internal::Device> device;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed)
          return Answer({ErrorCode::Cancelled, "context stopped"});
        if (!state_->device)
          state_->device = gpu_internal::Device::create(
              std::make_shared<ResourceBudget>(state_->budget));
        device = state_->device;
      }
      auto native = execution_internal::prepare_native_allocations(p, device);
      if (!native.ok())
        return Answer(native.status());
      prepared->native = native.take_value();
    }
    const auto reserve = [&](auto& values, std::size_t size) {
      const auto previous = values.capacity();
      values.reserve(size);
      core_internal::ResourceMetadataAccess::commit(
          prepared->metadata,
          (values.capacity() - previous) * sizeof(values[0]));
    };
    reserve(prepared->layouts, p.layouts.size());
    reserve(prepared->block_work, p.blocks.size());
    reserve(prepared->kernels, p.tasks.size());
    reserve(prepared->reads, p.tasks.size());
    reserve(prepared->output_ports, p.tasks.size());
    for (const auto& layout : p.layouts)
      prepared->layouts.push_back(kernel_layout(layout));
    prepared->kernels.resize(p.tasks.size());
    prepared->reads.resize(p.tasks.size());
    prepared->output_ports.resize(p.tasks.size());
    for (std::size_t ti = 0; ti < p.tasks.size(); ++ti) {
      const auto& task = p.tasks[ti];
      const auto& placement = p.placements[task.placement];
      if (placement.backend == physical::Backend::Gpu) {
        if (!state_->gpu_count)
          return Answer({ErrorCode::BackendUnavailable,
                         "GPU submission placement is unavailable"});
        prepared->has_gpu = true;
      }
      const auto* binding =
          task.call == physical::none ? nullptr : &plan.calls()[task.call];
      if (task.kind == physical::TaskKind::Copy &&
          placement.backend == physical::Backend::Gpu) {
        auto device = prepared->native->device;
        auto raw = device->prepare_copy(p.layouts[task.reads[0].layout]);
        if (!raw.ok())
          return Answer(raw.status());
        auto kernel =
            PreparedKernel::adopt(raw.value(), device, state_->budget);
        if (!kernel.ok()) {
          raw.value().release(raw.value().context);
          return Answer(kernel.status());
        }
        prepared->kernels[ti] = kernel.take_value();
      } else if (task.kind != physical::TaskKind::Copy) {
        const auto device =
            prepared->native ? prepared->native->device : nullptr;
        auto service =
            device ? device->preparation_service() : ps_native_gpu_service_v2{};
        auto kernel = binding->prepared->prepare_kernel(
            p, static_cast<physical::Id>(ti), binding->outputs,
            device ? &service : nullptr, device, state_->budget);
        if (!kernel.ok())
          return Answer(kernel.status());
        prepared->kernels[ti] = kernel.take_value();
      }
      auto& reads = prepared->reads[ti];
      reserve(reads, task.reads.size());
      reserve(prepared->output_ports[ti], task.writes.size());
      for (const auto& read : task.reads)
        reads.push_back(read.tile);
      std::sort(reads.begin(), reads.end());
      reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
      for (auto tile : task.writes) {
        auto port = physical::none;
        if (binding) {
          auto found = std::find(binding->outputs.begin(),
                                 binding->outputs.end(), p.tiles[tile].value);
          if (found != binding->outputs.end())
            port = static_cast<std::uint32_t>(found - binding->outputs.begin());
        }
        prepared->output_ports[ti].push_back(port);
      }
      prepared->maximum_reads =
          std::max(prepared->maximum_reads, task.reads.size());
      prepared->maximum_writes =
          std::max(prepared->maximum_writes, task.writes.size());
    }
    for (const auto& block : p.blocks) {
      prepared->maximum_calls =
          std::max(prepared->maximum_calls, block.calls.size());
      std::uint64_t work =
          1 + p.tasks.size() + p.calls.size() + 3 * p.tiles.size();
      auto add = [&](std::uint64_t value) {
        if (!core_internal::checked_add(work, value, &work))
          throw std::bad_alloc();
      };
      for (auto ti : block.tasks) {
        const auto& task = p.tasks[ti];
        add(5 + 2 * task.reads.size() + 2 * task.writes.size() +
            task.successors.size());
        if (task.kind == physical::TaskKind::Copy) {
          const auto& layout = p.layouts[task.reads[0].layout];
          std::uint64_t samples = 1;
          for (unsigned axis = 0; axis < layout.rank; ++axis)
            if (!core_internal::checked_multiply(samples, layout.shape[axis],
                                                 &samples))
              throw std::bad_alloc();
          add(samples);
        }
      }
      for (auto ci : block.calls) {
        add(3);
        for (auto vi : p.calls[ci].returns)
          add(2 * p.values[vi].tiles.size());
      }
      for (const auto& edge : block.edges) {
        for (auto vi : edge.arguments)
          add(2 * p.values[vi].tiles.size());
        for (auto vi : edge.live_values)
          add(2 * p.values[vi].tiles.size());
      }
      for (auto vi : block.return_values)
        add(2 * p.values[vi].tiles.size());
      prepared->block_work.push_back(work);
    }
    if (!plan.current())
      return Answer(stale());
    PreparedPlan result;
    result.state_ = std::move(prepared);
    return Answer(std::move(result));
  } catch (const std::bad_alloc&) {
    return Answer({ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Answer({ErrorCode::OperationFailed, {}});
  }
}
Result<RunHandle> ExecutionContext::submit(
    const PreparedPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, ExecutionOptions options) {
  using Answer = Result<RunHandle>;
  if (!plan.state_ || plan.state_->context != state_ || !plan.plan().current())
    return Answer(stale());
  if (!options.maximum_block_activations || !options.maximum_work ||
      options.maximum_parallelism > 4096)
    return Answer({ErrorCode::InvalidArgument, "invalid Run limits"});
  try {
    const auto lanes =
        options.maximum_parallelism
            ? std::min(options.maximum_parallelism, state_->cpu_count)
            : state_->cpu_count;
    const auto gpu_lanes =
        plan.state_->has_gpu
            ? (options.maximum_parallelism
                   ? std::min(options.maximum_parallelism, state_->gpu_count)
                   : state_->gpu_count)
            : 0;
    const auto& p = plan.plan().program();
    std::uint64_t schedule_bytes = 0;
    for (physical::Id block = 0; block < p.blocks.size(); ++block)
      schedule_bytes =
          std::max(schedule_bytes,
                   execution_internal::TaskSchedule::metadata_bytes(p, block));
    std::uint64_t frame_bytes =
        execution_internal::TileView::metadata_bytes +
        p.tiles.size() * (2 * sizeof(execution_internal::TileOwner) +
                          sizeof(std::uint32_t) + 1);
    auto lane_bytes =
        sizeof(RunState::Work) + 512 +
        plan.state_->maximum_reads *
            (sizeof(ps_kernel_const_view_v8) + sizeof(std::uint64_t)) +
        plan.state_->maximum_writes * sizeof(ps_kernel_mutable_view_v8);
    if (!core_internal::checked_multiply_add(
            p.values.size(),
            3 * sizeof(std::shared_ptr<execution_internal::ValueIdentity>) +
                3 * sizeof(ResultRef),
            frame_bytes, &frame_bytes))
      return Answer({ErrorCode::ResourceExhausted, "Run metadata overflow"});
    if (!core_internal::checked_multiply_add(lanes + gpu_lanes, lane_bytes,
                                             frame_bytes, &frame_bytes) ||
        !core_internal::checked_add(frame_bytes, schedule_bytes, &frame_bytes))
      return Answer({ErrorCode::ResourceExhausted, "Run metadata overflow"});
    auto retained_bytes =
        sizeof(RunState) + 1024 +
        p.tiles.size() * sizeof(execution_internal::TileOwner) +
        p.values.size() *
            (2 * sizeof(std::shared_ptr<execution_internal::ValueIdentity>) +
             sizeof(ResultRef)) +
        std::max<std::size_t>(1, plan.state_->maximum_calls) *
            sizeof(std::optional<execution_internal::DeliveryRecord>);
    if (!core_internal::checked_multiply_add(plan.state_->maximum_calls, 512,
                                             frame_bytes, &frame_bytes))
      return Answer({ErrorCode::ResourceExhausted, "Run metadata overflow"});
    std::uint64_t total;
    if (!core_internal::checked_add(frame_bytes, retained_bytes, &total))
      return Answer({ErrorCode::ResourceExhausted, "Run metadata overflow"});
    // Bootstrap the independent counters before attributing this Run's
    // reservations. Their lease holds only a weak counter reference.
    core_internal::ResourceMetadataScope bootstrap(state_->budget, {});
    std::uint64_t observation_bytes;
    if (!core_internal::checked_multiply_add(
            options.trace_capacity,
            sizeof(ExecutionTaskTiming) + sizeof(ExecutionCallTiming),
            sizeof(execution_internal::DiagnosticsState) +
                sizeof(core_internal::MetadataObservation) + 64,
            &observation_bytes))
      return Answer({ErrorCode::ResourceExhausted, "trace metadata overflow"});
    auto observation_lease = state_->budget.reserve(
        ResourceCapacity::host(observation_bytes, observation_bytes));
    if (!observation_lease.ok())
      return Answer(observation_lease.status());
    auto diagnostics = std::make_shared<execution_internal::DiagnosticsState>();
    diagnostics->metadata = observation_lease.take_value();
    core_internal::ResourceMetadataAccess::commit(
        diagnostics->metadata, sizeof(execution_internal::DiagnosticsState));
    diagnostics->metadata_observation =
        std::make_shared<core_internal::MetadataObservation>();
    core_internal::ResourceMetadataAccess::commit(
        diagnostics->metadata, sizeof(core_internal::MetadataObservation));
    core_internal::ResourceMetadataAccess::attribute(
        diagnostics->metadata, diagnostics->metadata_observation);
    core_internal::ResourceMetadataScope observation(
        state_->budget, diagnostics->metadata_observation);
    auto reserved =
        state_->budget.reserve(ResourceCapacity::host(total, total));
    if (!reserved.ok())
      return Answer(reserved.status());
    auto bound = bind_inputs(*plan.state_, bindings, reserved.value());
    if (!bound.ok())
      return Answer(bound.status());
    auto run = std::make_shared<RunState>();
    run->metadata = reserved.take_value();
    core_internal::ResourceMetadataAccess::commit(run->metadata,
                                                  sizeof(RunState));
    for (auto* status : {&run->abort_status, &run->block_status}) {
      status->message.reserve(PS_KERNEL_MESSAGE_CAPACITY_V8);
      core_internal::ResourceMetadataAccess::commit(
          run->metadata, status->message.capacity() + 1);
    }
    run->diagnostics = std::move(diagnostics);
    run->result.diagnostics.host_epoch_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            run->epoch.time_since_epoch())
            .count());
    run->diagnostics->snapshot.host_epoch_ns =
        run->result.diagnostics.host_epoch_ns;
    run->diagnostics->tasks.resize(options.trace_capacity);
    core_internal::ResourceMetadataAccess::commit(
        run->diagnostics->metadata,
        run->diagnostics->tasks.capacity() * sizeof(ExecutionTaskTiming));
    run->diagnostics->calls.resize(options.trace_capacity);
    core_internal::ResourceMetadataAccess::commit(
        run->diagnostics->metadata,
        run->diagnostics->calls.capacity() * sizeof(ExecutionCallTiming));
    run->frame_metadata = frame_bytes;
    run->prepared = plan.state_;
    run->lanes = options.maximum_parallelism
                     ? std::min(options.maximum_parallelism, state_->cpu_count)
                     : state_->cpu_count;
    run->gpu_lanes = gpu_lanes;
    run->options = std::move(options);
    run->remaining_work = run->options.maximum_work;
    run->cancellation = cancellation;
    run->initial = bound.take_value();
    run->input_identities = run->initial.identities;
    core_internal::ResourceMetadataAccess::commit(
        run->metadata,
        run->input_identities.capacity() * sizeof(run->input_identities[0]));
    run->records.resize(std::max<std::size_t>(1, run->prepared->maximum_calls));
    core_internal::ResourceMetadataAccess::commit(
        run->metadata, run->records.capacity() * sizeof(run->records[0]));
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed)
      return Answer({ErrorCode::Cancelled, "context stopped"});
    state_->runs.erase(
        std::remove_if(state_->runs.begin(), state_->runs.end(),
                       [](const auto& weak) {
                         auto run = weak.lock();
                         if (!run)
                           return true;
                         std::lock_guard<std::mutex> lock(run->mutex);
                         return run->terminal;
                       }),
        state_->runs.end());
    if (state_->runs.size() >= state_->maximum_runs ||
        state_->next_run == UINT64_MAX)
      return Answer({ErrorCode::ResourceExhausted, "context Run limit"});
    run->id = state_->next_run++;
    state_->runs.push_back(run);
    try {
      std::thread([run] { run->run(); }).detach();
    } catch (...) {
      state_->runs.pop_back();
      throw;
    }
    RunHandle result;
    result.state_ = std::move(run);
    return Answer(std::move(result));
  } catch (const std::bad_alloc&) {
    return Answer({ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Answer({ErrorCode::OperationFailed, {}});
  }
}
const ExecutionPlan& PreparedPlan::plan() const {
  if (!state_)
    throw std::logic_error("invalid prepared plan");
  return state_->plan;
}
bool RunHandle::request_cancel() const noexcept {
  return state_ && state_->cancel();
}
ExecutionObservation RunHandle::observation() const noexcept {
  ExecutionObservation result;
  if (state_)
    result.state_ = state_->diagnostics;
  return result;
}
ExecutionDiagnostics ExecutionObservation::snapshot() const noexcept {
  if (!state_)
    return {};
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto result = state_->snapshot;
  result.memory = state_->memory.snapshot();
  result.metadata = state_->metadata_observation->snapshot();
  return result;
}
std::size_t ExecutionObservation::copy_task_timings(
    ExecutionTaskTiming* target, std::size_t capacity) const noexcept {
  if (!state_ || !target)
    return 0;
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto count = std::min(capacity, state_->task_count);
  std::copy_n(state_->tasks.data(), count, target);
  return count;
}
std::size_t ExecutionObservation::copy_call_timings(
    ExecutionCallTiming* target, std::size_t capacity) const noexcept {
  if (!state_ || !target)
    return 0;
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto count = std::min(capacity, state_->call_count);
  std::copy_n(state_->calls.data(), count, target);
  return count;
}
Result<ExecutionResult> RunHandle::join() const {
  if (!state_)
    return Result<ExecutionResult>(
        {ErrorCode::InvalidArgument, "invalid Run handle"});
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (!state_->terminal && state_->delivery_id == std::this_thread::get_id())
    return Result<ExecutionResult>(
        {ErrorCode::InvalidArgument, "observer cannot join its own Run"});
  state_->changed.wait(lock, [&] { return state_->terminal; });
  if (!state_->abort_status.ok()) {
    try {
      return Result<ExecutionResult>(state_->abort_status);
    } catch (const std::bad_alloc&) {
      const auto& failure = state_->abort_status;
      return Result<ExecutionResult>(
          Status{failure.code, {}, failure.reason, failure.detail});
    }
  }
  try {
    auto result = state_->result;
    result.diagnostics.memory = state_->diagnostics->memory.snapshot();
    result.diagnostics.metadata =
        state_->diagnostics->metadata_observation->snapshot();
    return Result<ExecutionResult>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ExecutionResult>({ErrorCode::ResourceExhausted, {}});
  }
}
Result<ResourceBudget> ExecutionContext::resource_budget() const {
  return Result<ResourceBudget>(state_->budget);
}
std::uint32_t ExecutionContext::cpu_workers() const noexcept {
  return state_->cpu_count;
}
std::string ExecutionContext::native_device_description() const {
  std::shared_ptr<gpu_internal::Device> device;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    device = state_->device;
  }
  return device ? device->description() : std::string{};
}
}  // namespace ps
