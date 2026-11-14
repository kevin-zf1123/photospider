#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "host/provider_library.hpp"
#include "photospider/core/callback_environment.hpp"
#include "sdk/execution/provider.hpp"

namespace ps::executor_sdk {
namespace {
struct ObserverScope;
// A nested submission may deliver another Run on this thread.
thread_local const ObserverScope* current_observer = nullptr;
struct ObserverScope {
  explicit ObserverScope(const ObserverDelivery* delivery)
      : delivery(delivery), previous(std::exchange(current_observer, this)) {}
  ~ObserverScope() { current_observer = previous; }
  const ObserverDelivery* delivery;
  const ObserverScope* previous;
};
}  // namespace
bool ObserverDelivery::on_current_thread() const noexcept {
  for (auto scope = current_observer; scope; scope = scope->previous)
    if (scope->delivery == this)
      return true;
  return false;
}
Status ObserverDelivery::deliver(
    const std::function<void(const CallRecord&)>& observer,
    const CallRecord& record) {
  std::lock_guard<std::mutex> lock(mutex);
  if (failure.ok()) {
    try {
      ObserverScope scope(this);
      observer(record);
    } catch (...) {
      failure.code = ErrorCode::OperationFailed;
      failure.reason = FailureReason::HostException;
      failure.detail.origin = FailureOrigin::Observer;
      failure.detail.scope = FailureScope::Run;
      failure.detail.node_id = record.source_call;
      failure.detail.call_sequence = record.sequence;
    }
  }
  return failure;
}
Status ObserverDelivery::outcome() const {
  std::lock_guard<std::mutex> lock(mutex);
  return failure;
}
struct Provider {
  std::shared_ptr<void> library;
  const ps_executor_module_v2* api;
};
struct Owner {
  std::shared_ptr<const Provider> provider;
  ps_executor_owner_v1 object{};
  void release() {
    if (object.release) {
      auto entry = object;
      object = {};
      entry.release(entry.object);
    }
  }
  ~Owner() { release(); }
  const ps_executor_module_v2& api() const { return *provider->api; }
};
struct Context : Owner {
  ~Context() { release(); }
  std::shared_ptr<Environment> environment;
  ps_executor_host_v1 host{};
};
struct Prepared : Owner {
  ~Prepared() { release(); }
  std::shared_ptr<Context> context;
  ExecutionPlan plan;
};
struct Observation : Owner {
  std::shared_ptr<Environment> environment;
  ~Observation() { release(); }
};
struct Run : Owner {
  ~Run() { release(); }
  std::shared_ptr<const Prepared> prepared;

  std::shared_ptr<Observation> observation;
  std::shared_ptr<ObserverDelivery> delivery;
};
}  // namespace ps::executor_sdk
namespace ps {
namespace {
template <class F>
Status call(F fn) noexcept {
  try {
    core_internal::CallbackEnvironment env;
    return kernel_status(fn());
  } catch (const std::bad_alloc&) {
    return {ErrorCode::ResourceExhausted, {}};
  } catch (...) {
    return {ErrorCode::OperationFailed, {}};
  }
}
Status owner_status(const Status& status, const ps_executor_owner_v1& owner) {
  if (status.ok() && (!owner.object || !owner.release))
    return {ErrorCode::InvalidArgument, "empty executor owner"};
  return status;
}
ps_string_v8 view(const std::string& str) {
  return {str.data(), static_cast<uint32_t>(str.size())};
}
ps_kernel_status_v8 bad() {
  return kernel_failure(ErrorCode::InvalidArgument);
}
ps_executor_plan_v1 plan_view(const ExecutionPlan& p) {
  ps_executor_plan_v1 view_record{
      sizeof(ps_executor_plan_v1),
      5,
      static_cast<uint32_t>(p.execution_mode()),
      static_cast<uint32_t>(p.program().tasks.size()),
      static_cast<uint32_t>(p.input_declarations().size()),
      static_cast<uint32_t>(p.outputs().size()),
      p.tile_extent(),
      &p,
      [](const void* raw, uint32_t i, ps_string_v8* s) {
        const auto& plan = *static_cast<const ExecutionPlan*>(raw);
        if (!s || i >= plan.input_declarations().size())
          return bad();
        *s = view(plan.input_declarations()[i].name);
        return ps_kernel_status_v8{};
      },
      [](const void* raw, uint32_t i, ps_string_v8* s) {
        const auto& plan = *static_cast<const ExecutionPlan*>(raw);
        if (!s || i >= plan.outputs().size())
          return bad();
        *s = view(std::next(plan.outputs().begin(), i)->first);
        return ps_kernel_status_v8{};
      },
      [](const void* raw, uint32_t i, uint32_t* kind, uint32_t* backend,
         uint32_t* domain, uint32_t* abi) {
        const auto& program = static_cast<const ExecutionPlan*>(raw)->program();
        if (i >= program.tasks.size() || !kind || !backend || !domain || !abi)
          return bad();
        const auto& task = program.tasks[i];
        const auto& loc = program.placements[task.placement];
        *kind = static_cast<uint32_t>(task.kind);
        *backend = static_cast<uint32_t>(loc.backend);
        *domain = static_cast<uint32_t>(loc.domain);
        *abi = task.kernel == physical::none
                   ? 0
                   : program.kernels[task.kernel].abi_version;
        return ps_kernel_status_v8{};
      },
      nullptr,
      nullptr,
      nullptr,
      nullptr};
  executor_sdk::bind_plan_services(&view_record);
  return view_record;
}
}  // namespace
Status ExecutorRegistry::add_builtin(const ps_executor_module_v2& api) {
  providers_.emplace("physical", std::make_shared<executor_sdk::Provider>(
                                     executor_sdk::Provider{{}, &api}));
  return {};
}
Status ExecutorRegistry::load_plugin(const std::string& path) try {
  if (frozen_)
    return {ErrorCode::InvalidArgument, "executor catalog frozen"};
  auto library = host_internal::ProviderLibrary::open(path);
  if (!library.ok())
    return library.status();
  auto owner = library.take_value();
  auto table = host_internal::query_provider<ps_executor_module_v2>(
      *owner, "ps_executor_module_get_api_v2",
      PS_EXECUTOR_MODULE_ABI_VERSION_2);
  if (!table.ok())
    return table.status();
  auto m = table.value();
  auto name = host_internal::provider_text(m->name);
  if (!name.ok())
    return name.status();
  if (!m->create || !m->close || !m->prepare || !m->submit || !m->cancel ||
      !m->join || !m->observe || !m->snapshot || !m->trace ||
      !m->release_cache || !m->workers || !m->description ||
      !m->physical_version || !m->backend_mask || !m->domain_mask ||
      !m->invocation_mask)
    return {ErrorCode::InvalidArgument, "incomplete executor table"};
  if (!providers_
           .emplace(name.take_value(), std::make_shared<executor_sdk::Provider>(
                                           executor_sdk::Provider{owner, m}))
           .second)
    return {ErrorCode::InvalidArgument, "duplicate executor provider"};
  return {};
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
} catch (...) {
  return {ErrorCode::OperationFailed, "executor entry failed"};
}
ExecutionContext::ExecutionContext(std::shared_ptr<OperationRegistry> ops,
                                   ExecutionContextConfig cfg,
                                   const ExecutorRegistry& providers,
                                   const std::string& name) {
  auto it = providers.providers_.find(name);
  if (!ops || !ops->frozen() || !providers.frozen_ ||
      it == providers.providers_.end() || !cfg.maximum_runs ||
      cfg.maximum_runs > 65536 || !cfg.maximum_queued_tasks ||
      cfg.cpu_workers > 4096 || cfg.maximum_gpu_submissions > 4096 ||
      !cfg.maximum_live_bytes)
    throw std::invalid_argument("invalid executor configuration or provider: " +
                                name);
  auto limits = cfg.managed_resources.value_or(ResourceLimits{});
  limits.capacity[ResourceKind::Payload] =
      std::min(limits.capacity[ResourceKind::Payload], cfg.maximum_live_bytes);
  state_ = std::make_shared<executor_sdk::Context>();
  state_->provider = it->second;
  state_->environment =
      std::make_shared<executor_sdk::Environment>(executor_sdk::Environment{
          std::move(ops), cfg, ResourceBudget(limits, it->second->library),
          it->second->library});
  state_->host = {sizeof(ps_executor_host_v1),
                  1,
                  state_->environment.get(),
                  cfg.cpu_workers,
                  cfg.maximum_queued_tasks,
                  cfg.maximum_gpu_submissions,
                  cfg.maximum_runs,
                  cfg.maximum_live_bytes,
                  cfg.tile_cache_bytes,
                  [](const void* p, ps_executor_owner_v1* o) {
                    if (!p || !o)
                      return bad();
                    try {
                      *o = {new ResultRef(*static_cast<const ResultRef*>(p)),
                            [](void* r) { delete static_cast<ResultRef*>(r); }};
                      return ps_kernel_status_v8{};
                    } catch (...) {
                      return kernel_failure(ErrorCode::ResourceExhausted);
                    }
                  },
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr};
  executor_sdk::bind_data_services(&state_->host);
  auto status =
      owner_status(call([&] {
                     return state_->api().create(
                         state_->api().context, &state_->host, &state_->object);
                   }),
                   state_->object);
  if (!status.ok()) {
    if (status.code == ErrorCode::ResourceExhausted)
      throw std::bad_alloc();
    throw std::invalid_argument(status.message);
  }
}
ExecutionContext::~ExecutionContext() noexcept {
  if (state_)
    state_->api().close(state_->object.object);
}
Result<PreparedPlan> ExecutionContext::prepare(const ExecutionPlan& plan) const
    try {
  using Answer = Result<PreparedPlan>;
  if (!plan.current() || !plan.bound_to(*state_->environment->operations))
    return Answer({ErrorCode::Stale, "foreign or stale Plan"});
  const auto& api = state_->api();
  if (api.physical_version != 5)
    return Answer({ErrorCode::TypeMismatch, "physical Plan version"});
  const auto& program = plan.program();
  for (const auto& placement : program.placements)
    if (!(api.backend_mask &
          (1U << static_cast<unsigned>(placement.backend))) ||
        !(api.domain_mask & (1U << static_cast<unsigned>(placement.domain))))
      return Answer(
          {ErrorCode::BackendUnavailable, "executor placement unsupported"});
  for (const auto& tile : program.tiles)
    if (!(api.domain_mask & (1U << static_cast<unsigned>(tile.domain))))
      return Answer(
          {ErrorCode::BackendUnavailable, "executor tile domain unsupported"});
  for (const auto& kernel : program.kernels)
    if (kernel.abi_version >= 32 ||
        !(api.invocation_mask & (1U << kernel.abi_version)))
      return Answer(
          {ErrorCode::TypeMismatch, "executor invocation ABI unsupported"});
  auto owner = std::make_shared<executor_sdk::Prepared>();
  owner->provider = state_->provider;
  owner->context = state_;
  owner->plan = plan;
  auto q = plan_view(owner->plan);
  auto status = owner_status(call([&] {
                               return api.prepare(state_->object.object, &q,
                                                  &owner->object);
                             }),
                             owner->object);
  if (!status.ok())
    return Answer(status);
  PreparedPlan result;
  result.state_ = std::move(owner);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<PreparedPlan>({ErrorCode::ResourceExhausted, {}});
}
const ExecutionPlan& PreparedPlan::plan() const {
  if (!state_)
    throw std::logic_error("invalid preparation");
  return state_->plan;
}
Result<RunHandle> ExecutionContext::submit(const PreparedPlan& plan,
                                           ExecutionBindings bindings,
                                           const CancellationToken& cancel,
                                           ExecutionOptions options) try {
  using Answer = Result<RunHandle>;
  if (!plan.state_ || plan.state_->context != state_ || !plan.plan().current())
    return Answer({ErrorCode::Stale, "foreign or stale preparation"});
  if (state_->provider->library) {
    auto status = executor_sdk::validate_bindings(plan.plan(), bindings);
    if (!status.ok())
      return Answer(status);
  }
  auto r = std::make_shared<executor_sdk::Run>();
  r->provider = state_->provider;
  r->prepared = plan.state_;
  r->observation = std::make_shared<executor_sdk::Observation>();
  r->observation->provider = r->provider;
  r->observation->environment = state_->environment;
  auto submission = std::make_shared<executor_sdk::Submission>(
      plan.plan(), std::move(bindings), cancel, std::move(options));
  const auto& o = submission->options;
  if (o.on_call_retired && state_->provider->library) {
    submission->delivery = std::make_shared<executor_sdk::ObserverDelivery>();
    r->delivery = submission->delivery;
  }
  ps_executor_submit_v2 request = {
      sizeof(request),
      static_cast<uint32_t>(submission->bindings.inputs.size()),
      o.maximum_parallelism,
      o.trace_capacity,
      o.maximum_work,
      o.maximum_refinement_stages,
      o.maximum_block_activations,
      submission.get(),
      [](const void* raw, uint32_t i, ps_string_v8* name, const void** result) {
        const auto& b =
            static_cast<const executor_sdk::Submission*>(raw)->bindings.inputs;
        if (!name || !result || i >= b.size())
          return bad();
        *name = view(b[i].name);
        *result = &b[i].result;
        return ps_kernel_status_v8{};
      },
      [](const void* raw) -> uint32_t {
        return static_cast<const executor_sdk::Submission*>(raw)
            ->cancellation.cancelled();
      },
      [](const void* p, ps_executor_owner_v1* out) {
        if (!p || !out)
          return bad();
        try {
          using Ref = std::shared_ptr<const executor_sdk::Submission>;
          auto owner = static_cast<const executor_sdk::Submission*>(p)
                           ->shared_from_this();
          *out = {new Ref(std::move(owner)),
                  [](void* v) { delete static_cast<Ref*>(v); }};
          return ps_kernel_status_v8{};
        } catch (...) {
          return kernel_failure(ErrorCode::ResourceExhausted);
        }
      },
      [](const void* p, uint64_t seq, uint64_t activation, uint64_t source,
         uint32_t call_id, uint32_t outcome, uint32_t failed_task,
         ps_kernel_status_v8 result, const uint32_t* ids,
         const void* const* values, uint32_t count) {
        if (!p || outcome > PS_EXECUTOR_CALL_ELIDED_V2 ||
            (count && (!ids || !values)))
          return bad();
        try {
          const auto& submission =
              *static_cast<const executor_sdk::Submission*>(p);
          const auto& program = submission.plan.program();
          if (call_id >= program.calls.size() ||
              source != program.calls[call_id].source_call)
            return bad();
          auto status = kernel_status(result);
          if (outcome == PS_EXECUTOR_CALL_FAILED_V2) {
            if (failed_task >= program.tasks.size() ||
                program.tasks[failed_task].call != call_id || status.ok() ||
                count)
              return bad();
            status.detail.origin = FailureOrigin::Backend;
            status.detail.scope = FailureScope::Run;
            status.detail.node_id = source;
            status.detail.task_id = failed_task;
          } else if (failed_task != PS_EXECUTOR_TASK_NONE_V2 || !status.ok() ||
                     (outcome == PS_EXECUTOR_CALL_ELIDED_V2 && count)) {
            return bad();
          }
          if (outcome == PS_EXECUTOR_CALL_SUCCEEDED_V2 &&
              count != program.calls[call_id].returns.size())
            return bad();
          CallRecord record;
          record.sequence = seq;
          record.activation = activation;
          record.source_call = source;
          record.call = call_id;
          record.outcome = static_cast<CallOutcome>(outcome);
          record.failed_task = failed_task;
          record.status = std::move(status);
          for (unsigned i = 0; i < count; ++i) {
            if (!values[i] ||
                !record.values
                     .emplace(ids[i], *static_cast<const ResultRef*>(values[i]))
                     .second)
              return bad();
          }
          if (record.outcome == CallOutcome::Succeeded) {
            auto valid =
                executor_sdk::validate_exports(submission.plan, record);
            if (!valid.ok())
              return kernel_failure(valid);
          }
          if (submission.delivery)
            return kernel_failure(submission.delivery->deliver(
                submission.options.on_call_retired, record));
          return ps_kernel_status_v8{};
        } catch (const std::bad_alloc&) {
          return kernel_failure(ErrorCode::ResourceExhausted);
        } catch (...) {
          return kernel_failure(ErrorCode::OperationFailed, "observer failed");
        }
      }};
  auto status =
      owner_status(call([&] {
                     return r->api().submit(state_->object.object,
                                            plan.state_->object.object,
                                            &request, &r->object);
                   }),
                   r->object);
  if (!status.ok()) {
    if (r->delivery && status.code != ErrorCode::Cancelled &&
        status.code != ErrorCode::Stale) {
      auto observer = r->delivery->outcome();
      if (!observer.ok())
        return Answer(observer);
    }
    return Answer(status);
  }
  status = owner_status(call([&] {
                          return r->api().observe(r->object.object,
                                                  &r->observation->object);
                        }),
                        r->observation->object);
  if (!status.ok()) {
    r->api().cancel(r->object.object);
    RunHandle pending;
    pending.state_ = r;
    (void)pending.join();
    return Answer(status);
  }
  RunHandle result;
  result.state_ = std::move(r);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<RunHandle>({ErrorCode::ResourceExhausted, {}});
}
bool RunHandle::request_cancel() const noexcept {
  return state_ && state_->api().cancel(state_->object.object);
}
ExecutionObservation RunHandle::observation() const noexcept {
  ExecutionObservation o;
  if (state_)
    o.state_ = state_->observation;
  return o;
}
ExecutionDiagnostics ExecutionObservation::snapshot() const noexcept {
  ExecutionDiagnostics d;
  if (state_)
    state_->api().snapshot(state_->object.object, &d);
  return d;
}
std::size_t ExecutionObservation::copy_task_timings(
    ExecutionTaskTiming* p, std::size_t n) const noexcept {
  return state_ && p && n
             ? std::min<uint64_t>(
                   n, state_->api().trace(state_->object.object, 0, p, n))
             : 0;
}
std::size_t ExecutionObservation::copy_call_timings(
    ExecutionCallTiming* p, std::size_t n) const noexcept {
  return state_ && p && n
             ? std::min<uint64_t>(
                   n, state_->api().trace(state_->object.object, 1, p, n))
             : 0;
}
Result<ExecutionResult> RunHandle::join() const try {
  using Answer = Result<ExecutionResult>;
  if (!state_)
    return Answer({ErrorCode::InvalidArgument, "empty run"});
  if (state_->delivery && state_->delivery->on_current_thread())
    return Answer(
        {ErrorCode::InvalidArgument, "cannot join a Run from its observer"});
  struct Published {
    ExecutionResult value;
    Status status;
  };
  Published out;
  ps_executor_failure_v1 detail{};
  ps_executor_results_v1 sink{
      &out,
      [](void* c, ps_string_v8 name, const void* r) {
        auto& out = *static_cast<Published*>(c);
        if (out.status.ok()) {
          try {
            auto key = host_internal::provider_text(name);
            if (!key.ok() || !r ||
                !out.value.results
                     .emplace(key.value(), *static_cast<const ResultRef*>(r))
                     .second)
              out.status = {ErrorCode::InvalidArgument,
                            "invalid executor result"};
          } catch (...) {
            out.status = {ErrorCode::ResourceExhausted, {}};
          }
        }
        return kernel_failure(out.status);
      },
      &out.value.diagnostics, &detail, &out.value};
  Status status;
  {
    core_internal::CallbackEnvironment env;
    ps_kernel_status_v8 encoded{};
    try {
      encoded = state_->api().join(state_->object.object, &sink);
    } catch (const std::bad_alloc&) {
      return Answer({ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return Answer({ErrorCode::OperationFailed, {}});
    }
    // The terminal outcome is already latched. Failure to allocate the
    // caller's diagnostic text must preserve its structured fields.
    try {
      assign_kernel_status(&status, encoded);
    } catch (const std::bad_alloc&) {
      status.message.clear();
    }
  }
  // Providers settle the delivery lane before join returns. A callback
  // exception is still observable when a provider ignores the retire result.
  if (state_->delivery && status.code != ErrorCode::Cancelled &&
      status.code != ErrorCode::Stale) {
    auto observer = state_->delivery->outcome();
    if (!observer.ok())
      return Answer(observer);
  }
  if (!status.ok()) {
    status.detail.origin = static_cast<FailureOrigin>(detail.origin);
    status.detail.scope = static_cast<FailureScope>(detail.scope);
    status.detail.node_id = detail.node;
    status.detail.input_id = detail.input;
    if (detail.flags & 1) {
      AtomKey atom{detail.output_index, detail.rank, {}};
      std::copy_n(detail.coordinate, 8, atom.coordinate.begin());
      if (atom.canonical())
        status.detail.atom = atom;
    }
    if (detail.flags & 2)
      status.detail.call_sequence = detail.sequence;
    if (detail.flags & 4)
      status.detail.task_id = detail.task;
    return Answer(std::move(status));
  }
  if (!out.status.ok())
    return Answer(std::move(out.status));
  if (state_->provider->library) {
    auto valid =
        executor_sdk::validate_results(state_->prepared->plan, out.value);
    if (!valid.ok())
      return Answer(valid);
  }
  return Answer(std::move(out.value));
} catch (const std::bad_alloc&) {
  return Result<ExecutionResult>({ErrorCode::ResourceExhausted, {}});
}
Result<ResourceBudget> ExecutionContext::resource_budget() const {
  return Result<ResourceBudget>(state_->environment->budget);
}
bool ExecutionContext::release_cached_tiles() const {
  return state_->api().release_cache(state_->object.object);
}
uint32_t ExecutionContext::cpu_workers() const noexcept {
  return state_->api().workers(state_->object.object);
}
std::string ExecutionContext::native_device_description() const {
  std::string text;
  auto status = call([&] {
    return state_->api().description(
        state_->object.object, &text, [](void* c, ps_string_v8 s) {
          if (s.size)
            static_cast<std::string*>(c)->assign(s.data, s.size);
        });
  });
  if (status.code == ErrorCode::ResourceExhausted)
    throw std::bad_alloc();
  return text;
}
}  // namespace ps
