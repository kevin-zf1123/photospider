// Photospider kernel: Scheduler Plugin Loader implementation
#include "scheduler/scheduler_plugin_loader.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"
#include "photospider/scheduler/scheduler_plugin_api.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ps {

namespace {

/**
 * @brief Wraps one native dynamic-library handle in shared lifetime ownership.
 *
 * @param handle Non-null handle returned by `LoadLibrary` or `dlopen`.
 * @return Shared lifetime whose final release unloads `handle`.
 * @throws std::bad_alloc if the shared ownership control block cannot allocate.
 * @note Scheduler owners retain a copy through their plugin destroy call; the
 * platform unload function is invoked only when the final copy is released.
 */
std::shared_ptr<void> make_library_lifetime(void* handle) {
#ifdef _WIN32
  return std::shared_ptr<void>(handle, [](void* ptr) {
    if (ptr) {
      FreeLibrary(static_cast<HMODULE>(ptr));
    }
  });
#else
  return std::shared_ptr<void>(handle, [](void* ptr) {
    if (ptr) {
      dlclose(ptr);
    }
  });
#endif
}

/**
 * @brief Tracks exact host task exceptions without replacing their dynamic
 * type.
 *
 * Each task handle, callback, or explicit exception publication receives a
 * preallocated slot before plugin entry. When host work throws, the relay
 * stores `std::current_exception()` in that slot and rethrows the original
 * exception. A later scheduler boundary compares the surfaced `exception_ptr`
 * identity against registered slots before deciding whether DSO normalization
 * applies.
 *
 * @throws std::bad_alloc when creating or retaining a new slot cannot allocate.
 * @note Slot recording and identity lookup use a non-allocating spin guard, so
 * the original host failure is never replaced by wrapper construction or
 * registry allocation. The plugin observes the original exception type,
 * `what()`, and rethrow behavior.
 */
class HostTaskExceptionRegistry final {
 public:
  /**
   * @brief Holds one preallocated host-exception identity through plugin wait.
   * @throws Nothing.
   * @note The registry creates the slot before plugin entry and reads or
   * mutates it only while holding the registry spin guard. Slots remain
   * append-only until `clear()` swaps the complete collection out; recorded
   * exception objects are then destroyed after the guard is released.
   */
  struct Slot final {
    /** @brief Original host exception, populated before or during plugin work.
     */
    std::exception_ptr error;
    /** @brief Whether a live relay or explicit publication owns this index. */
    bool active = false;
  };

  /** @brief Stable numeric slot identity unaffected by vector reallocation. */
  using SlotIndex = std::size_t;

  /**
   * @brief Constructs an empty host task exception registry.
   * @throws Nothing.
   */
  HostTaskExceptionRegistry() noexcept = default;

  /**
   * @brief Releases registered host exception identities.
   * @throws Nothing.
   * @note Destruction occurs only after the plugin instance has been destroyed.
   */
  ~HostTaskExceptionRegistry() noexcept = default;

  /**
   * @brief Prevents copying synchronized exception identity state.
   * @param other Registry that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  HostTaskExceptionRegistry(const HostTaskExceptionRegistry& other) = delete;

  /**
   * @brief Prevents assignment across synchronized exception registries.
   * @param other Registry that cannot replace this registry.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  HostTaskExceptionRegistry& operator=(const HostTaskExceptionRegistry& other) =
      delete;

  /**
   * @brief Allocates and registers one host exception slot.
   * @param initial Optional exception already entering plugin first-error
   * state.
   * @return Stable slot index retained by relay state.
   * @throws std::bad_alloc if registry growth fails.
   * @note Allocation finishes before the plugin can observe the corresponding
   * task, callback, or exception pointer.
   */
  SlotIndex create_slot(std::exception_ptr initial = nullptr) {
    SpinGuard guard(lock_);
    slots_.push_back(Slot{std::move(initial), true});
    return slots_.size() - 1U;
  }

  /**
   * @brief Allocates a group of reusable indices for one handle batch.
   * @param count Number of valid handles that can each throw once.
   * @param indices Output vector receiving exactly `count` active indices.
   * @return Nothing.
   * @throws std::bad_alloc if output or registry growth fails.
   * @throws std::length_error if the requested slot count is not representable.
   * @note Output capacity is reserved before acquiring the spin guard. Slots
   * are append-only until the matching wait clears the registry, so each
   * admission is O(count) and never scans prior active batches. Registry growth
   * uses one vector operation and avoids per-task allocation/control blocks.
   */
  void create_slots(SlotIndex count, std::vector<SlotIndex>* indices) {
    const SlotIndex original_output_size = indices->size();
    if (count > indices->max_size() - original_output_size) {
      throw std::length_error("Scheduler host task slot count overflow");
    }
    indices->reserve(original_output_size + count);

    SpinGuard guard(lock_);
    const SlotIndex original_slot_size = slots_.size();
    if (count > slots_.max_size() - original_slot_size) {
      throw std::length_error("Scheduler host task registry overflow");
    }
    slots_.resize(original_slot_size + count);
    for (SlotIndex offset = 0; offset < count; ++offset) {
      indices->push_back(original_slot_size + offset);
      Slot& slot = slots_[original_slot_size + offset];
      slot.error = nullptr;
      slot.active = true;
    }
  }

  /**
   * @brief Records one thrown host exception in a preallocated slot.
   * @param slot Stable slot index created before plugin entry.
   * @param error Non-null original exception from `std::current_exception()`.
   * @return Nothing.
   * @throws Nothing; no allocation occurs and the slot accepts only its first
   * exception pointer.
   * @note First-write publication never destroys a prior exception object while
   * the registry guard is held.
   */
  void record(SlotIndex slot, std::exception_ptr error) noexcept {
    SpinGuard guard(lock_);
    if (slot < slots_.size() && slots_[slot].active &&
        slots_[slot].error == nullptr) {
      slots_[slot].error = std::move(error);
    }
  }

  /**
   * @brief Tests whether one surfaced exception is registered host work.
   * @param error Exception currently escaping a scheduler plugin call.
   * @return True only for the same non-null exception object identity.
   * @throws Nothing; lookup allocates no storage.
   */
  bool contains(const std::exception_ptr& error) const noexcept {
    if (error == nullptr) {
      return false;
    }
    SpinGuard guard(lock_);
    return std::any_of(slots_.begin(), slots_.end(), [&](const Slot& slot) {
      return slot.active && slot.error != nullptr && slot.error == error;
    });
  }

  /**
   * @brief Retires one slot whose submission did not retain host work.
   * @param slot Slot index to retire after pre-entry construction failure.
   * @return Nothing.
   * @throws Nothing.
   * @note Retirement only changes scalar visibility. Any recorded
   * `exception_ptr` remains retained until lock-free destruction by `clear()`.
   */
  void retire(SlotIndex slot) noexcept {
    SpinGuard guard(lock_);
    if (slot < slots_.size()) {
      slots_[slot].active = false;
    }
  }

  /**
   * @brief Retires all slot indices owned by a rejected or settled handle
   * batch.
   * @param slots Stable indices allocated together before plugin entry.
   * @return Nothing.
   * @throws Nothing.
   * @note Rejected entries remain append-only residue until the matching wait;
   * no exception object is released while the registry guard is held.
   */
  void retire(const std::vector<SlotIndex>& slots) noexcept {
    SpinGuard guard(lock_);
    for (const SlotIndex slot : slots) {
      if (slot < slots_.size()) {
        slots_[slot].active = false;
      }
    }
  }

  /**
   * @brief Retires every host exception identity after batch settlement.
   * @return Nothing.
   * @throws Nothing.
   * @note The matching plugin wait has returned or thrown before this reset, so
   * no accepted callback may publish a later first exception for that batch.
   * The vector swap is non-throwing; exception objects are destroyed only after
   * the spin guard is released, permitting user destructor reentry.
   */
  void clear() noexcept {
    std::vector<Slot> retired;
    {
      SpinGuard guard(lock_);
      retired.swap(slots_);
    }
    retired.clear();
    {
      SpinGuard guard(lock_);
      if (slots_.empty()) {
        slots_.swap(retired);
      }
    }
  }

 private:
  /**
   * @brief Non-allocating guard for the short registry critical sections.
   * @throws Nothing.
   * @note Plugin code is never invoked while this guard is held.
   */
  class SpinGuard final {
   public:
    /**
     * @brief Acquires one registry flag.
     * @param lock Flag guarding slot membership and exception-pointer values.
     * @throws Nothing.
     */
    explicit SpinGuard(std::atomic_flag& lock) noexcept : lock_(lock) {
      while (lock_.test_and_set(std::memory_order_acquire)) {
      }
    }

    /** @brief Releases the registry flag. @throws Nothing. */
    ~SpinGuard() noexcept { lock_.clear(std::memory_order_release); }

    /**
     * @brief Prevents duplicate ownership of the same lock.
     * @param other Guard that cannot be copied.
     * @throws Nothing because the operation is deleted.
     */
    SpinGuard(const SpinGuard&) = delete;
    /**
     * @brief Prevents assignment across active lock ownership.
     * @param other Guard that cannot replace this active guard.
     * @return No value because the operation is deleted.
     * @throws Nothing because the operation is deleted.
     */
    SpinGuard& operator=(const SpinGuard&) = delete;

   private:
    /** @brief Borrowed registry flag released at scope exit. */
    std::atomic_flag& lock_;
  };

  /** @brief Serializes slot membership and pointer publication without throws.
   */
  mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
  /** @brief Indexed slots retained until rejection cleanup or matching wait. */
  std::vector<Slot> slots_;
};

/**
 * @brief Validates one fixed public scheduler device enumerator.
 * @param device Device value returned by a scheduler implementation.
 * @return True for CPU, Metal GPU, CUDA GPU, or ASIC/NPU.
 * @throws Nothing.
 * @note Unknown numeric values cannot enter host compute planning even though
 * the transitional C++ ABI permits a hostile DSO to manufacture them.
 */
bool is_valid_scheduler_device(Device device) noexcept {
  switch (device) {
    case Device::CPU:
    case Device::GPU_METAL:
    case Device::GPU_CUDA:
    case Device::ASIC_NPU:
      return true;
  }
  return false;
}

/**
 * @brief Calls one scheduler DSO entry behind an exception-normalization fence.
 *
 * @tparam Callback Host callable entering one validated scheduler plugin.
 * @param library_lifetime DSO lease copied for the complete invocation and
 * exception-inspection interval.
 * @param callback Entry call to execute while the lease is retained.
 * @return The callback result, including `void`.
 * @param host_task_exceptions Optional registry of original host task failures
 * that may surface from this runtime call.
 * @throws Any registered host task exception with its original dynamic type.
 * @throws std::bad_alloc as a fresh host-owned standard exception.
 * @throws GraphError copied into host code for plugin `GraphError`, invalid
 * argument, other standard, and unknown exceptions.
 * @note No plugin-origin exception object or dynamic type leaves this frame.
 * `GraphError` preserves code/message, `std::invalid_argument` maps to
 * `InvalidParameter`, and other ordinary or unknown failures map to
 * `ComputeError`.
 */
template <typename Callback>
decltype(auto) invoke_scheduler_plugin_boundary(
    const std::shared_ptr<void>& library_lifetime,
    const HostTaskExceptionRegistry* host_task_exceptions,
    Callback&& callback) {
  const std::shared_ptr<void> invocation_lifetime = library_lifetime;
  (void)invocation_lifetime;
  try {
    return std::forward<Callback>(callback)();
  } catch (...) {
    const std::exception_ptr error = std::current_exception();
    if (host_task_exceptions != nullptr &&
        host_task_exceptions->contains(error)) {
      std::rethrow_exception(error);
    }
    try {
      std::rethrow_exception(error);
    } catch (const std::bad_alloc&) {
      throw std::bad_alloc{};
    } catch (const GraphError& graph_error) {
      throw GraphError(graph_error.code(), graph_error.what());
    } catch (const std::invalid_argument& invalid_argument) {
      throw GraphError(GraphErrc::InvalidParameter, invalid_argument.what());
    } catch (const std::exception& standard_error) {
      throw GraphError(GraphErrc::ComputeError, standard_error.what());
    } catch (...) {
      throw GraphError(GraphErrc::ComputeError,
                       "Scheduler plugin callback failed with an unknown "
                       "exception");
    }
  }
}

/**
 * @brief Calls one plugin boundary without host-task identity restoration.
 * @tparam Callback Host callable entering plugin discovery or implementation.
 * @param library_lifetime DSO lease retained through exception inspection.
 * @param callback Plugin call to execute.
 * @return Callback result, including `void`.
 * @throws std::bad_alloc or GraphError after plugin-origin normalization.
 */
template <typename Callback>
decltype(auto) invoke_scheduler_plugin(
    const std::shared_ptr<void>& library_lifetime, Callback&& callback) {
  return invoke_scheduler_plugin_boundary(library_lifetime, nullptr,
                                          std::forward<Callback>(callback));
}

/**
 * @brief Calls a scheduler runtime method and restores registered host tasks.
 * @tparam Callback Host callable entering one runtime virtual method.
 * @param library_lifetime DSO lease retained through exception inspection.
 * @param host_task_exceptions Registry populated by host task relays.
 * @param callback Runtime call to execute.
 * @return The callback result, including `void`.
 * @throws The exact original host task exception when pointer identity matches.
 * @throws std::bad_alloc or GraphError from plugin-origin normalization.
 */
template <typename Callback>
decltype(auto) invoke_scheduler_plugin_runtime(
    const std::shared_ptr<void>& library_lifetime,
    const HostTaskExceptionRegistry& host_task_exceptions,
    Callback&& callback) {
  return invoke_scheduler_plugin_boundary(library_lifetime,
                                          &host_task_exceptions,
                                          std::forward<Callback>(callback));
}

/**
 * @brief Registers host executor exceptions before rethrowing them unchanged.
 *
 * @throws Any target exception unchanged after its pointer identity is
 * recorded.
 * @note The relay remains host-owned until the matching plugin wait boundary;
 * the plugin receives only its borrowed public `TaskExecutor` base pointer.
 */
class PluginTaskExecutorRelay final : public TaskExecutor {
 public:
  /**
   * @brief Binds one borrowed host executor.
   * @param target Non-null executor from the original task handle.
   * @param registry Host registry retained by the scheduler owner.
   * @param slot Preallocated identity slot for this task handle.
   * @throws Nothing.
   */
  PluginTaskExecutorRelay(TaskExecutor* target,
                          HostTaskExceptionRegistry* registry,
                          HostTaskExceptionRegistry::SlotIndex slot) noexcept
      : target_(target), registry_(registry), slot_(slot) {}

  /**
   * @brief Transfers a borrowed executor pointer during batch construction.
   * @param other Relay whose target moves here.
   * @throws Nothing.
   */
  PluginTaskExecutorRelay(PluginTaskExecutorRelay&& other) noexcept
      : target_(std::exchange(other.target_, nullptr)),
        registry_(std::exchange(other.registry_, nullptr)),
        slot_(std::exchange(other.slot_, 0U)) {}

  /**
   * @brief Replaces this borrowed target during vector relocation.
   * @param other Relay whose target moves here.
   * @return This relay.
   * @throws Nothing.
   */
  PluginTaskExecutorRelay& operator=(PluginTaskExecutorRelay&& other) noexcept {
    target_ = std::exchange(other.target_, nullptr);
    registry_ = std::exchange(other.registry_, nullptr);
    slot_ = std::exchange(other.slot_, 0U);
    return *this;
  }

  /**
   * @brief Prevents duplicate relay ownership of one batch slot.
   * @param other Relay that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  PluginTaskExecutorRelay(const PluginTaskExecutorRelay& other) = delete;

  /**
   * @brief Prevents copy assignment between relay slots.
   * @param other Relay that cannot be copied.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  PluginTaskExecutorRelay& operator=(const PluginTaskExecutorRelay& other) =
      delete;

  /**
   * @brief Releases the borrowed executor pointer without deleting it.
   * @throws Nothing.
   */
  ~PluginTaskExecutorRelay() noexcept override = default;

  /**
   * @brief Executes one host task and records any original failure identity.
   * @param task_id Dense host task identifier.
   * @return Nothing.
   * @throws Any host task exception unchanged after non-allocating
   * registration.
   * @throws GraphError if a moved-from relay is invoked by a broken plugin.
   */
  void run_task(int task_id) override {
    if (target_ == nullptr) {
      throw GraphError(GraphErrc::ComputeError,
                       "Scheduler plugin invoked a retired task relay");
    }
    try {
      target_->run_task(task_id);
    } catch (...) {
      registry_->record(slot_, std::current_exception());
      throw;
    }
  }

 private:
  /** @brief Borrowed original executor owned by the compute dispatcher. */
  TaskExecutor* target_ = nullptr;
  /** @brief Borrowed host registry that outlives this submitted relay. */
  HostTaskExceptionRegistry* registry_ = nullptr;
  /** @brief Preallocated identity slot index recorded on task failure. */
  HostTaskExceptionRegistry::SlotIndex slot_ = 0U;
};

/**
 * @brief Owns host executor relays for one submitted task-handle batch.
 *
 * @throws std::bad_alloc if relay or transformed-handle storage cannot grow.
 * @note Executor storage is reserved completely before emplacement, so every
 * pointer published in `handles_` remains stable until this batch is retired.
 * A construction failure after slot admission explicitly retires every
 * admitted index because a not-yet-constructed batch has no destructor.
 */
class PluginTaskHandleBatch final {
 public:
  /**
   * @brief Replaces valid handles with host-owned executor relays.
   * @param handles Original borrowed handles transferred by the dispatcher.
   * @param registry Host identity registry retained through matching wait.
   * @throws std::bad_alloc if exact-size reservation or slot creation fails.
   * @throws Any element-construction failure after retiring admitted slots.
   * @note Empty handles are preserved and remain no-ops. All vector capacity is
   * reserved before slot admission; the catch fence remains an explicit strong
   * cleanup guarantee if `TaskHandle` or relay construction changes later.
   */
  PluginTaskHandleBatch(std::vector<TaskHandle>&& handles,
                        HostTaskExceptionRegistry* registry)
      : registry_(registry) {
    const std::size_t valid_handle_count =
        static_cast<std::size_t>(std::count_if(
            handles.begin(), handles.end(), [](const TaskHandle& handle) {
              return static_cast<bool>(handle);
            }));
    executors_.reserve(handles.size());
    handles_.reserve(handles.size());
    slots_.reserve(valid_handle_count);
    registry_->create_slots(valid_handle_count, &slots_);
    try {
      std::size_t valid_handle_index = 0U;
      for (const TaskHandle& handle : handles) {
        if (!handle) {
          handles_.push_back(handle);
          continue;
        }
        executors_.emplace_back(handle.executor, registry_,
                                slots_[valid_handle_index]);
        ++valid_handle_index;
        handles_.push_back(
            TaskHandle{&executors_.back(), handle.task_id, handle.node_id});
      }
    } catch (...) {
      registry_->retire(slots_);
      throw;
    }
  }

  /**
   * @brief Releases host relay storage after the plugin borrowing interval.
   * @throws Nothing.
   */
  ~PluginTaskHandleBatch() noexcept { registry_->retire(slots_); }

  /**
   * @brief Moves transformed handles into one plugin batch call.
   * @return Handles borrowing stable relays owned by this object.
   * @throws Nothing.
   */
  std::vector<TaskHandle> take_handles() noexcept {
    return std::move(handles_);
  }

 private:
  /** @brief Borrowed registry that outlives this retained batch. */
  HostTaskExceptionRegistry* registry_ = nullptr;
  /** @brief Stable host executor relays, one per valid original handle. */
  std::vector<PluginTaskExecutorRelay> executors_;
  /** @brief Handles published to the plugin and moved out exactly once. */
  std::vector<TaskHandle> handles_;
  /** @brief Registry indices retired when this batch borrowing interval ends.
   */
  std::vector<HostTaskExceptionRegistry::SlotIndex> slots_;
};

/**
 * @brief Registers one host callback failure and rethrows it unchanged.
 * @param task Non-empty host callback transferred by the dispatcher.
 * @param registry Host identity registry retained through matching wait.
 * @param slot Preallocated identity slot for this callback.
 * @return Host-owned callback preserving the target exception type and
 * identity.
 * @throws std::bad_alloc if `std::function` target storage cannot allocate.
 */
SchedulerTaskRuntime::Task relay_host_task_callback(
    SchedulerTaskRuntime::Task&& task, HostTaskExceptionRegistry* registry,
    HostTaskExceptionRegistry::SlotIndex slot) {
  return [task = std::move(task), registry, slot]() mutable {
    try {
      task();
    } catch (...) {
      registry->record(slot, std::current_exception());
      throw;
    }
  };
}

/**
 * @brief Invokes one plugin destroy export behind a no-throw ABI fence.
 *
 * @param scheduler Raw plugin scheduler whose ownership is being released.
 * @param destroy Plugin export paired with `scheduler`.
 * @return Nothing.
 * @throws Nothing; every plugin exception, including `std::bad_alloc`, is
 * suppressed because cleanup callers are `noexcept`.
 * @note The export is invoked exactly once. A hostile export that throws before
 * ending the object lifetime is not retried because a second ABI call could
 * double-destroy partially released plugin state. The caller must retain the
 * dynamic-library lifetime until this function returns.
 */
void destroy_plugin_scheduler_noexcept(IScheduler* scheduler,
                                       void (*destroy)(IScheduler*)) noexcept {
  try {
    destroy(scheduler);
  } catch (...) {
    // Plugin exceptions cannot cross a host cleanup/destructor boundary.
  }
}

/**
 * @brief Host-side owner/delegator for one plugin-created scheduler.
 *
 * The wrapper delegates the complete inherited `IScheduler` contract, destroys
 * the raw scheduler through the plugin ABI, and retains the dynamic library
 * until that destroy call has completed. Every plugin-origin exception is
 * inspected while this lease is live and converted to a host-owned exception;
 * host task failures are registered by `exception_ptr` identity, remain
 * unchanged inside plugin code, and retain their exact original identity when
 * surfaced back to the host.
 *
 * @note `library_` is the final member and therefore remains alive throughout
 * the destructor body. Construction may allocate for the wrapper and copied
 * type name; `RawPluginSchedulerGuard` owns the raw instance until construction
 * succeeds.
 */
class PluginSchedulerOwner final : public IScheduler {
 public:
  /**
   * @brief Takes ownership of a validated plugin scheduler.
   * @param scheduler Raw instance implementing the complete `IScheduler`
   * contract, including its statically inherited task runtime.
   * @param destroy Plugin ABI destroy function paired with `scheduler`.
   * @param library Shared library lifetime retained through destruction.
   * @param type_name Registered scheduler type copied for `name()`.
   * @throws std::bad_alloc if `type_name` copy allocation fails.
   * @note The caller retains a temporary raw-instance guard until this
   * constructor and owner allocation both succeed.
   */
  PluginSchedulerOwner(IScheduler* scheduler, void (*destroy)(IScheduler*),
                       std::shared_ptr<void> library, std::string type_name)
      : type_name_(std::move(type_name)),
        scheduler_(scheduler),
        destroy_(destroy),
        library_(std::move(library)) {}

  /**
   * @brief Prevents duplication of plugin instance and destroy ownership.
   * @param other Owner whose raw instance cannot be shared by value.
   * @throws Nothing because the operation is deleted.
   */
  PluginSchedulerOwner(const PluginSchedulerOwner& other) = delete;

  /**
   * @brief Prevents assignment across plugin instance ownership boundaries.
   * @param other Owner whose raw instance cannot replace this instance.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  PluginSchedulerOwner& operator=(const PluginSchedulerOwner& other) = delete;

  /**
   * @brief Best-effort stops, detaches, and plugin-destroys the owned instance.
   *
   * Lifecycle fallback stages are fenced independently. Ownership is cleared
   * before plugin calls begin, and the destroy export is invoked exactly once
   * even when shutdown or detach fails.
   *
   * @throws Nothing; all plugin lifecycle and destroy exceptions are
   * suppressed.
   * @note `library_` remains alive throughout this destructor body and is
   * released only after the single destroy attempt returns.
   */
  ~PluginSchedulerOwner() noexcept override {
    IScheduler* scheduler = std::exchange(scheduler_, nullptr);
    if (scheduler) {
      if (!shutdown_attempted_) {
        try {
          scheduler->shutdown();
        } catch (...) {
        }
      }
      if (!detach_attempted_) {
        try {
          scheduler->detach();
        } catch (...) {
        }
      }
      destroy_plugin_scheduler_noexcept(scheduler, destroy_);
    }
  }

  /**
   * @brief Explicitly attaches the plugin scheduler to a host context.
   * @param host Borrowed host context forwarded unchanged.
   * @return Nothing.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   * @note Destructor fallback is re-armed before entering plugin code. If a
   * second attach throws after partially retaining `host`, owner destruction
   * therefore still attempts detach behind its no-throw fence.
   */
  void attach(SchedulerHostContext& host) override {
    detach_attempted_ = false;
    invoke_scheduler_plugin(library_, [&]() { scheduler_->attach(host); });
  }
  /**
   * @brief Explicitly detaches the plugin scheduler.
   * @return Nothing.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   * @note The attempt is recorded before entering plugin code, so destruction
   * does not repeat a transition that may have partially completed. Any other
   * still-unattempted cleanup stage retains its independent no-throw fence.
   */
  void detach() override {
    detach_attempted_ = true;
    invoke_scheduler_plugin(library_, [&]() { scheduler_->detach(); });
  }
  /**
   * @brief Explicitly starts the plugin scheduler.
   * @return Nothing.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   * @note Destructor fallback is re-armed before entering plugin code. A
   * failed restart may already own workers or other partial runtime state, so
   * owner destruction must still attempt shutdown.
   */
  void start() override {
    shutdown_attempted_ = false;
    invoke_scheduler_plugin(library_, [&]() { scheduler_->start(); });
  }
  /**
   * @brief Explicitly shuts down the plugin scheduler.
   * @return Nothing.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   * @note The attempt is recorded before entering plugin code, so destruction
   * does not repeat a transition that may have partially completed. Detach
   * remains independently fenced when it has not yet been attempted.
   */
  void shutdown() override {
    shutdown_attempted_ = true;
    invoke_scheduler_plugin(library_, [&]() { scheduler_->shutdown(); });
  }

  /**
   * @brief Returns the host-owned registered scheduler type.
   * @return Copied type name retained independently of plugin metadata storage.
   * @throws std::bad_alloc if return-value construction cannot allocate.
   */
  std::string name() const override { return type_name_; }
  /**
   * @brief Delegates runtime statistics to the plugin scheduler.
   * @return Plugin-provided statistics string.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   */
  std::string get_stats() const override {
    return invoke_scheduler_plugin(library_,
                                   [&]() { return scheduler_->get_stats(); });
  }
  /**
   * @brief Queries the plugin scheduler lifecycle state.
   * @return Plugin-reported running flag.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   */
  bool is_running() const override {
    return invoke_scheduler_plugin(library_,
                                   [&]() { return scheduler_->is_running(); });
  }

  /**
   * @brief Delegates runtime device discovery to the plugin scheduler.
   * @return Plugin-provided device list in its native preference order.
   * @throws std::bad_alloc as a fresh host-owned resource failure.
   * @throws GraphError after normalizing every other plugin-origin exception.
   * @note Forwarding is required because the base implementation reports CPU
   * only and would otherwise hide accelerator capabilities from compute
   * planning.
   */
  std::vector<Device> available_devices() const override {
    std::vector<Device> devices = invoke_scheduler_plugin(
        library_, [&]() { return require_task_runtime().available_devices(); });
    if (!std::all_of(devices.begin(), devices.end(),
                     is_valid_scheduler_device)) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Scheduler plugin returned an unknown device value");
    }
    return devices;
  }

  /**
   * @brief Delegates one initial borrowed-handle batch to the plugin runtime.
   * @param handles Initial ready task handles transferred to the plugin.
   * @param total_task_count Total completion count for the new batch.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws The exact host task exception when a synchronously executing plugin
   * surfaces one relayed handle failure.
   * @throws std::bad_alloc or GraphError after normalizing plugin-origin
   * failures.
   * @note Direct delegation preserves the plugin runtime's transactional batch
   * semantics instead of using the base closure-conversion fallback.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    auto batch = retain_task_handle_batch(std::move(handles));
    try {
      invoke_scheduler_plugin_runtime(
          library_, task_exception_registry_, [&]() {
            require_task_runtime().submit_initial_task_handles(
                batch->take_handles(), total_task_count, priority);
          });
    } catch (...) {
      release_task_handle_batch(batch);
      throw;
    }
  }

  /**
   * @brief Delegates a worker-origin borrowed-handle batch to the plugin.
   * @param handles Ready task handles transferred as one batch.
   * @param priority Scheduler-supported priority hint.
   * @return Nothing.
   * @throws The exact host task exception when a synchronously executing plugin
   * surfaces one relayed handle failure.
   * @throws std::bad_alloc or GraphError after normalizing plugin-origin
   * failures.
   * @note Direct delegation prevents the base implementation from publishing a
   * prefix through repeated single-handle submissions.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    auto batch = retain_task_handle_batch(std::move(handles));
    try {
      invoke_scheduler_plugin_runtime(
          library_, task_exception_registry_, [&]() {
            require_task_runtime().submit_ready_task_handles_from_worker(
                batch->take_handles(), priority);
          });
    } catch (...) {
      release_task_handle_batch(batch);
      throw;
    }
  }

  /**
   * @brief Delegates one caller-thread ready callback to the plugin runtime.
   * @param task Ready callback transferred to the plugin.
   * @param priority Scheduler-supported priority hint.
   * @param epoch Optional plugin batch epoch.
   * @return Nothing.
   * @throws The exact host callback exception when a synchronously executing
   * plugin surfaces the same registered exception.
   * @throws std::bad_alloc or GraphError after normalizing plugin-origin
   * failures.
   */
  void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    if (!task) {
      invoke_scheduler_plugin(library_, [&]() {
        require_task_runtime().submit_ready_task_any_thread(std::move(task),
                                                            priority, epoch);
      });
      return;
    }
    auto slot = task_exception_registry_.create_slot();
    Task relayed_task;
    try {
      relayed_task = relay_host_task_callback(std::move(task),
                                              &task_exception_registry_, slot);
    } catch (...) {
      task_exception_registry_.retire(slot);
      throw;
    }
    try {
      invoke_scheduler_plugin_runtime(
          library_, task_exception_registry_, [&]() {
            require_task_runtime().submit_ready_task_any_thread(
                std::move(relayed_task), priority, epoch);
          });
    } catch (...) {
      task_exception_registry_.retire(slot);
      throw;
    }
  }

  /**
   * @brief Waits for the plugin task runtime to complete its active batch.
   * @return Nothing.
   * @throws The exact first host task exception matched by registered pointer
   * identity.
   * @throws std::bad_alloc or GraphError after normalizing a plugin-origin wait
   * failure.
   * @note Return or throw retires all host executor relays borrowed by the
   * plugin for this batch.
   */
  void wait_for_completion() override {
    try {
      invoke_scheduler_plugin_runtime(
          library_, task_exception_registry_,
          [&]() { require_task_runtime().wait_for_completion(); });
    } catch (...) {
      clear_task_relays();
      throw;
    }
    clear_task_relays();
  }

  /**
   * @brief Publishes one task exception to the plugin runtime.
   * @param e Original host exception identity registered before plugin storage;
   * null input remains null and mutates no owner-side task state.
   * @return Nothing.
   * @throws The exact host exception if a plugin synchronously rethrows the
   * registered pointer.
   * @throws std::bad_alloc from slot allocation or normalized plugin resource
   * failure.
   * @throws GraphError after normalizing other plugin-origin failures.
   */
  void set_exception(std::exception_ptr e) override {
    if (e == nullptr) {
      invoke_scheduler_plugin(
          library_, [&]() { require_task_runtime().set_exception(nullptr); });
      return;
    }
    const auto slot = task_exception_registry_.create_slot(e);
    try {
      invoke_scheduler_plugin_runtime(
          library_, task_exception_registry_,
          [&]() { require_task_runtime().set_exception(e); });
    } catch (...) {
      task_exception_registry_.retire(slot);
      throw;
    }
  }

  /**
   * @brief Increases plugin runtime completion accounting.
   * @param delta Count forwarded unchanged.
   * @return Nothing.
   * @throws std::bad_alloc or GraphError after normalizing plugin-origin
   * failures.
   */
  void inc_tasks_to_complete(int delta) override {
    invoke_scheduler_plugin(library_, [&]() {
      require_task_runtime().inc_tasks_to_complete(delta);
    });
  }

  /**
   * @brief Decrements plugin runtime completion accounting once.
   * @return Nothing.
   * @throws std::bad_alloc or GraphError after normalizing plugin-origin
   * failures.
   */
  void dec_tasks_to_complete() override {
    invoke_scheduler_plugin(
        library_, [&]() { require_task_runtime().dec_tasks_to_complete(); });
  }

  /**
   * @brief Forwards one scheduler trace event to the plugin runtime.
   * @param action Scheduler trace action.
   * @param node_id Associated graph node id.
   * @return Nothing.
   * @throws std::bad_alloc or GraphError if a hostile plugin violates the
   * non-throwing trace contract.
   */
  void log_event(SchedulerTraceAction action, int node_id) override {
    invoke_scheduler_plugin(
        library_, [&]() { require_task_runtime().log_event(action, node_id); });
  }

 private:
  /**
   * @brief Retains relayed executors for one plugin handle batch.
   * @param handles Original dispatcher handles transferred by the caller.
   * @return Shared batch kept both by the call frame and owner registry.
   * @throws std::bad_alloc if relay, handle, shared-owner, or registry storage
   * cannot allocate.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Publication into `task_handle_batches_` precedes plugin entry, so a
   * synchronously executing scheduler never observes a dangling relay.
   */
  std::shared_ptr<PluginTaskHandleBatch> retain_task_handle_batch(
      std::vector<TaskHandle>&& handles) {
    auto batch = std::make_shared<PluginTaskHandleBatch>(
        std::move(handles), &task_exception_registry_);
    std::lock_guard<std::mutex> lock(task_relay_mutex_);
    task_handle_batches_.push_back(batch);
    return batch;
  }

  /**
   * @brief Retires one rejected or synchronously failed handle batch.
   * @param batch Batch whose plugin submission did not complete successfully.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The call-frame shared owner remains alive until this helper returns
   * and the original normalized or host-task exception resumes propagation.
   */
  void release_task_handle_batch(
      const std::shared_ptr<PluginTaskHandleBatch>& batch) {
    std::lock_guard<std::mutex> lock(task_relay_mutex_);
    const auto retained = std::find(task_handle_batches_.begin(),
                                    task_handle_batches_.end(), batch);
    if (retained != task_handle_batches_.end()) {
      task_handle_batches_.erase(retained);
    }
  }

  /**
   * @brief Retires every borrowed executor relay after plugin wait completion.
   * @return Nothing.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note The public runtime contract ends every handle borrowing interval when
   * `wait_for_completion` returns or throws.
   */
  void clear_task_handle_batches() {
    std::lock_guard<std::mutex> lock(task_relay_mutex_);
    task_handle_batches_.clear();
  }

  /**
   * @brief Retires handle relays and registered task exception identities.
   * @return Nothing.
   * @throws std::system_error only if locking the handle-batch mutex fails.
   * @note The plugin wait boundary has settled accepted work before this call;
   * no registry lock is held while entering plugin code.
   */
  void clear_task_relays() {
    clear_task_handle_batches();
    task_exception_registry_.clear();
  }

  /**
   * @brief Returns the inherited task-runtime side of the plugin instance.
   * @return Borrowed runtime reference valid for this owner lifetime.
   * @throws Nothing.
   * @note Public inheritance makes runtime type discovery unnecessary.
   */
  SchedulerTaskRuntime& require_task_runtime() const noexcept {
    return *scheduler_;
  }

  /** @brief Host-owned scheduler type copied independently of plugin memory. */
  std::string type_name_;
  /** @brief Raw plugin scheduler destroyed only through `destroy_`. */
  IScheduler* scheduler_ = nullptr;
  /** @brief Plugin destroy export paired with `scheduler_`. */
  void (*destroy_)(IScheduler*) = nullptr;
  /**
   * @brief True after shutdown begins and until a later start attempt begins.
   * @note False requires destructor fallback even when the latest start threw.
   */
  bool shutdown_attempted_ = false;
  /**
   * @brief True after detach begins and until a later attach attempt begins.
   * @note False requires destructor fallback even when the latest attach threw.
   */
  bool detach_attempted_ = false;
  /** @brief Serializes owner-side lifetime storage for borrowed task relays. */
  std::mutex task_relay_mutex_;
  /** @brief Exact host task exception identities retained through plugin wait.
   */
  HostTaskExceptionRegistry task_exception_registry_;
  /** @brief Relayed handle batches retained through the matching wait. */
  std::vector<std::shared_ptr<PluginTaskHandleBatch>> task_handle_batches_;
  /** @brief DSO lifetime released after scheduler destroy returns. */
  std::shared_ptr<void> library_;
};

/**
 * @brief Stack owner for a raw scheduler returned by plugin create.
 *
 * The guard is established immediately after a non-null plugin instance is
 * returned and remains active through owner allocation and the copied
 * type-name construction performed by `PluginSchedulerOwner`. It performs no
 * dynamic allocation of its own.
 *
 * @note `library_` retains the plugin until after `destroy_` completes. Calling
 * `release()` transfers instance destruction to the fully constructed owner;
 * every exceptional exit before that point destroys the raw instance exactly
 * once through the plugin ABI.
 */
class RawPluginSchedulerGuard final {
 public:
  /**
   * @brief Starts guarding one plugin-created scheduler instance.
   *
   * @param scheduler Raw instance returned by the plugin create export.
   * @param destroy Plugin destroy export paired with `scheduler`.
   * @param library Shared dynamic-library lifetime already allocated at load.
   * @throws Nothing; copying `std::shared_ptr` only increments its control
   * block reference count.
   * @note `scheduler` and `destroy` must both be non-null.
   */
  RawPluginSchedulerGuard(IScheduler* scheduler, void (*destroy)(IScheduler*),
                          const std::shared_ptr<void>& library) noexcept
      : scheduler_(scheduler), destroy_(destroy), library_(library) {}

  /**
   * @brief Destroys an untransferred instance while its library is mapped.
   *
   * @throws Nothing; a hostile destroy-export exception is suppressed so the
   * original owner-construction exception continues unchanged.
   * @note The destroy export is attempted exactly once. The `library_` member
   * is released only after this destructor body and the fenced call complete.
   */
  ~RawPluginSchedulerGuard() noexcept {
    IScheduler* scheduler = std::exchange(scheduler_, nullptr);
    if (scheduler) {
      destroy_plugin_scheduler_noexcept(scheduler, destroy_);
    }
  }

  /**
   * @brief Prevents duplicating raw plugin-instance ownership.
   * @param other Guard whose destroy duty cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  RawPluginSchedulerGuard(const RawPluginSchedulerGuard& other) = delete;

  /**
   * @brief Prevents copy assignment of raw plugin-instance ownership.
   * @param other Guard whose destroy duty cannot replace this guard.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  RawPluginSchedulerGuard& operator=(const RawPluginSchedulerGuard& other) =
      delete;

  /**
   * @brief Transfers instance destruction to a completed owner.
   *
   * @return The guarded raw pointer.
   * @throws Nothing.
   * @note Call only after `PluginSchedulerOwner` construction succeeds.
   */
  IScheduler* release() noexcept {
    IScheduler* released = scheduler_;
    scheduler_ = nullptr;
    return released;
  }

 private:
  /** @brief Plugin instance destroyed unless ownership is released. */
  IScheduler* scheduler_ = nullptr;
  /** @brief Plugin ABI destroy function paired with `scheduler_`. */
  void (*destroy_)(IScheduler*) = nullptr;
  /** @brief Library lifetime retained through instance destruction. */
  std::shared_ptr<void> library_;
};

}  // namespace

/** @copydoc SchedulerPluginLoader::instance */
SchedulerPluginLoader& SchedulerPluginLoader::instance() {
  static SchedulerPluginLoader instance;
  return instance;
}

/** @copydoc SchedulerPluginLoader::~SchedulerPluginLoader */
SchedulerPluginLoader::~SchedulerPluginLoader() {
  loaded_plugins_.clear();
}

/** @copydoc SchedulerPluginLoader::RegistryState::RegistryState */
SchedulerPluginLoader::RegistryState::RegistryState(
    const SchedulerPluginLoader& loader)
    : loaded_plugins(  // NOLINT(whitespace/indent_namespace)
          loader.loaded_plugins_),
      type_to_plugin(  // NOLINT(whitespace/indent_namespace)
          loader.type_to_plugin_),
      type_info(  // NOLINT(whitespace/indent_namespace)
          loader.type_info_),
      load_errors(  // NOLINT(whitespace/indent_namespace)
          loader.load_errors_) {}

/** @copydoc SchedulerPluginLoader::RegistryState::commit */
void SchedulerPluginLoader::RegistryState::commit(
    SchedulerPluginLoader& loader) noexcept {
  static_assert(noexcept(loader.loaded_plugins_.swap(loaded_plugins)),
                "loaded plugin registry commit must not throw");
  static_assert(noexcept(loader.type_to_plugin_.swap(type_to_plugin)),
                "type-to-plugin registry commit must not throw");
  static_assert(noexcept(loader.type_info_.swap(type_info)),
                "type metadata registry commit must not throw");
  static_assert(noexcept(loader.load_errors_.swap(load_errors)),
                "load-error registry commit must not throw");
  loader.loaded_plugins_.swap(loaded_plugins);
  loader.type_to_plugin_.swap(type_to_plugin);
  loader.type_info_.swap(type_info);
  loader.load_errors_.swap(load_errors);
}

/** @copydoc SchedulerPluginLoader::append_load_error_unlocked */
void SchedulerPluginLoader::append_load_error_unlocked(std::string error) {
  std::vector<std::string> staged_errors = load_errors_;
  staged_errors.push_back(std::move(error));
  load_errors_.swap(staged_errors);
}

/** @copydoc SchedulerPluginLoader::scan_and_load(const std::string&) */
std::size_t SchedulerPluginLoader::scan_and_load(const std::string& dir_path) {
  return scan_and_load(std::vector<std::string>{dir_path});
}

/** @copydoc SchedulerPluginLoader::scan_and_load(const
 * std::vector<std::string>&) */
std::size_t SchedulerPluginLoader::scan_and_load(
    const std::vector<std::string>& dir_paths) {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t loaded_count = 0;

  for (const auto& dir_path : dir_paths) {
    bool recursive = false;
    std::string actual_path = dir_path;

    // Parse the supported directory-expression suffix.
    if (dir_path.size() >= 3 && dir_path.substr(dir_path.size() - 3) == "/**") {
      recursive = true;
      actual_path = dir_path.substr(0, dir_path.size() - 3);
    } else if (dir_path.size() >= 2 &&
               dir_path.substr(dir_path.size() - 2) == "/*") {
      actual_path = dir_path.substr(0, dir_path.size() - 2);
    }

    fs::path base_dir(actual_path);
    if (!fs::exists(base_dir) || !fs::is_directory(base_dir)) {
      continue;
    }

    auto process_entry = [&](const fs::directory_entry& entry) {
      if (!entry.is_regular_file())
        return;

      const auto& path = entry.path();
#if defined(_WIN32)
      const std::string extension = ".dll";
#elif defined(__APPLE__)
      const std::string extension = ".dylib";
#else
      const std::string extension = ".so";
#endif

      if (path.extension() != extension)
        return;

      // Skip absolute paths already retained by this registry.
      std::string abs_path = fs::absolute(path).string();
      if (loaded_plugins_.count(abs_path) > 0)
        return;

      if (load_plugin_internal_unlocked(path)) {
        ++loaded_count;
      }
    };

    if (recursive) {
      for (const auto& entry : fs::recursive_directory_iterator(base_dir)) {
        process_entry(entry);
      }
    } else {
      for (const auto& entry : fs::directory_iterator(base_dir)) {
        process_entry(entry);
      }
    }
  }

  return loaded_count;
}

/** @copydoc SchedulerPluginLoader::load_plugin */
bool SchedulerPluginLoader::load_plugin(const fs::path& plugin_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_plugin_internal_unlocked(plugin_path);
}

/** @copydoc SchedulerPluginLoader::load_plugin_internal_unlocked */
bool SchedulerPluginLoader::load_plugin_internal_unlocked(
    const fs::path& plugin_path) {
  std::string abs_path = fs::absolute(plugin_path).string();

  // Preserve idempotent loading for the normalized absolute path.
  if (loaded_plugins_.count(abs_path) > 0) {
    return true;
  }

  PluginHandle handle;
  handle.path = abs_path;

#ifdef _WIN32
  handle.handle = LoadLibrary(abs_path.c_str());
  if (!handle.handle) {
    append_load_error_unlocked("Failed to load plugin: " + abs_path +
                               " (LoadLibrary failed)");
    return false;
  }
  handle.library = make_library_lifetime(handle.handle);
  handle.get_abi_version = reinterpret_cast<SchedulerPluginGetAbiVersionFunc>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     kSchedulerPluginGetAbiVersionSymbol));
#else
  handle.handle = dlopen(abs_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle.handle) {
    const char* err = dlerror();
    append_load_error_unlocked("Failed to load plugin: " + abs_path + " (" +
                               (err ? err : "unknown error") + ")");
    return false;
  }
  handle.library = make_library_lifetime(handle.handle);
  dlerror();
  handle.get_abi_version = reinterpret_cast<SchedulerPluginGetAbiVersionFunc>(
      dlsym(handle.handle, kSchedulerPluginGetAbiVersionSymbol));
#endif

  if (handle.get_abi_version == nullptr) {
    append_load_error_unlocked(
        "Scheduler plugin missing ABI handshake: " + abs_path + " (need " +
        kSchedulerPluginGetAbiVersionSymbol + ")");
    return false;
  }
  const std::uint32_t abi_version = handle.get_abi_version();
  if (abi_version != PS_SCHEDULER_PLUGIN_ABI_VERSION) {
    append_load_error_unlocked("Scheduler plugin ABI mismatch: " + abs_path +
                               " (expected " +
                               std::to_string(PS_SCHEDULER_PLUGIN_ABI_VERSION) +
                               ", got " + std::to_string(abi_version) + ")");
    return false;
  }

#ifdef _WIN32
  handle.get_count =
      reinterpret_cast<SchedulerPluginGetCountFunc>(GetProcAddress(
          static_cast<HMODULE>(handle.handle), kSchedulerPluginGetCountSymbol));
  handle.get_name = reinterpret_cast<SchedulerPluginGetNameFunc>(GetProcAddress(
      static_cast<HMODULE>(handle.handle), kSchedulerPluginGetNameSymbol));
  handle.get_description = reinterpret_cast<SchedulerPluginGetDescriptionFunc>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     kSchedulerPluginGetDescriptionSymbol));
  handle.create = reinterpret_cast<SchedulerPluginCreateFunc>(GetProcAddress(
      static_cast<HMODULE>(handle.handle), kSchedulerPluginCreateSymbol));
  handle.destroy = reinterpret_cast<SchedulerPluginDestroyFunc>(GetProcAddress(
      static_cast<HMODULE>(handle.handle), kSchedulerPluginDestroySymbol));
  handle.get_version = reinterpret_cast<SchedulerPluginGetVersionFunc>(
      GetProcAddress(static_cast<HMODULE>(handle.handle),
                     kSchedulerPluginGetVersionSymbol));
#else
  handle.get_count = reinterpret_cast<SchedulerPluginGetCountFunc>(
      dlsym(handle.handle, kSchedulerPluginGetCountSymbol));
  handle.get_name = reinterpret_cast<SchedulerPluginGetNameFunc>(
      dlsym(handle.handle, kSchedulerPluginGetNameSymbol));
  handle.get_description = reinterpret_cast<SchedulerPluginGetDescriptionFunc>(
      dlsym(handle.handle, kSchedulerPluginGetDescriptionSymbol));
  handle.create = reinterpret_cast<SchedulerPluginCreateFunc>(
      dlsym(handle.handle, kSchedulerPluginCreateSymbol));
  handle.destroy = reinterpret_cast<SchedulerPluginDestroyFunc>(
      dlsym(handle.handle, kSchedulerPluginDestroySymbol));
  handle.get_version = reinterpret_cast<SchedulerPluginGetVersionFunc>(
      dlsym(handle.handle, kSchedulerPluginGetVersionSymbol));
#endif

  if (handle.get_count == nullptr || handle.get_name == nullptr ||
      handle.get_description == nullptr || handle.create == nullptr ||
      handle.destroy == nullptr || handle.get_version == nullptr) {
    append_load_error_unlocked(
        "Scheduler plugin missing required exports after ABI handshake: " +
        abs_path);
    return false;
  }

  const char* version = invoke_scheduler_plugin(
      handle.library, [&]() { return handle.get_version(); });
  handle.version = version != nullptr ? version : "unknown";

  RegistryState staged(*this);

  // Stage every type only after the ABI and full export set are validated.
  const std::uint32_t count = invoke_scheduler_plugin(
      handle.library, [&]() { return handle.get_count(); });
  if (count == 0U) {
    append_load_error_unlocked(
        "Scheduler plugin reported zero scheduler types: " + abs_path);
    return false;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const char* name = invoke_scheduler_plugin(
        handle.library, [&]() { return handle.get_name(i); });
    if (name == nullptr || name[0] == '\0') {
      append_load_error_unlocked(
          "Scheduler plugin returned an invalid scheduler type name at "
          "index " +
          std::to_string(i) + ": " + abs_path);
      return false;
    }

    std::string type_name = name;

    // Conflicts are diagnostics; non-conflicting types still register.
    if (builtins_.count(type_name) > 0) {
      staged.load_errors.push_back(
          "Scheduler type '" + type_name +
          "' conflicts with builtin in plugin: " + abs_path);
      continue;
    }
    const auto existing_type = staged.type_to_plugin.find(type_name);
    if (existing_type != staged.type_to_plugin.end()) {
      staged.load_errors.push_back(
          "Scheduler type '" + type_name + "' already registered by plugin: " +
          existing_type->second + " (in " + abs_path + ")");
      continue;
    }

    // Publish only into shadow registries until the complete scan succeeds.
    staged.type_to_plugin[type_name] = abs_path;
    handle.registered_types.push_back(type_name);

    // Copy all plugin-owned metadata while the candidate DSO is retained.
    SchedulerPluginInfo info;
    info.type_name = type_name;
    info.plugin_path = abs_path;
    info.version = handle.version;
    info.is_builtin = false;

    const char* desc = invoke_scheduler_plugin(
        handle.library, [&]() { return handle.get_description(i); });
    if (desc != nullptr) {
      info.description = desc;
    }

    staged.type_info[type_name] = std::move(info);
  }

  if (handle.registered_types.empty()) {
    append_load_error_unlocked(
        "Scheduler plugin registered no non-conflicting scheduler types: " +
        abs_path);
    return false;
  }

  // Retain the DSO inside the same no-throw registry commit.
  staged.loaded_plugins[abs_path] = std::move(handle);
  staged.commit(*this);

  return true;
}

/** @copydoc SchedulerPluginLoader::unload_plugin */
bool SchedulerPluginLoader::unload_plugin(const fs::path& plugin_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string abs_path = fs::absolute(plugin_path).string();
  auto it = loaded_plugins_.find(abs_path);
  if (it == loaded_plugins_.end()) {
    return false;
  }

  // Remove every type before releasing the loader-owned DSO lifetime.
  for (const auto& type_name : it->second.registered_types) {
    type_to_plugin_.erase(type_name);
    type_info_.erase(type_name);
  }

  loaded_plugins_.erase(it);
  return true;
}

/** @copydoc SchedulerPluginLoader::get_registered_types */
std::vector<std::string> SchedulerPluginLoader::get_registered_types() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::string> types;

  // Copy built-in types.
  for (const auto& [name, _] : builtins_) {
    types.push_back(name);
  }

  // Copy plugin types.
  for (const auto& [name, _] : type_to_plugin_) {
    types.push_back(name);
  }

  std::sort(types.begin(), types.end());
  return types;
}

/** @copydoc SchedulerPluginLoader::is_registered */
bool SchedulerPluginLoader::is_registered(const std::string& type_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return builtins_.count(type_name) > 0 || type_to_plugin_.count(type_name) > 0;
}

/** @copydoc SchedulerPluginLoader::get_info */
std::optional<SchedulerPluginInfo> SchedulerPluginLoader::get_info(
    const std::string& type_name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  // Built-in metadata is synthesized from host-owned state.
  auto builtin_it = builtins_.find(type_name);
  if (builtin_it != builtins_.end()) {
    SchedulerPluginInfo info;
    info.type_name = type_name;
    info.description = builtin_it->second.description;
    info.plugin_path = "(builtin)";
    info.version = "builtin";
    info.is_builtin = true;
    return info;
  }

  // Plugin metadata was copied and cached at load time.
  auto it = type_info_.find(type_name);
  if (it != type_info_.end()) {
    return it->second;
  }

  return std::nullopt;
}

/** @copydoc SchedulerPluginLoader::get_all_info */
std::vector<SchedulerPluginInfo> SchedulerPluginLoader::get_all_info() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<SchedulerPluginInfo> result;

  // Copy built-in metadata.
  for (const auto& [name, builtin] : builtins_) {
    SchedulerPluginInfo info;
    info.type_name = name;
    info.description = builtin.description;
    info.plugin_path = "(builtin)";
    info.version = "builtin";
    info.is_builtin = true;
    result.push_back(info);
  }

  // Copy cached plugin metadata without re-entering a DSO.
  for (const auto& [_, info] : type_info_) {
    result.push_back(info);
  }

  return result;
}

/** @copydoc SchedulerPluginLoader::get_description */
std::string SchedulerPluginLoader::get_description(
    const std::string& type_name) const {
  auto info = get_info(type_name);
  return info ? info->description : "Unknown scheduler type";
}

/** @copydoc SchedulerPluginLoader::create */
std::unique_ptr<IScheduler> SchedulerPluginLoader::create(
    const std::string& type_name, unsigned int num_workers) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Built-in factories remain host-owned.
  auto builtin_it = builtins_.find(type_name);
  if (builtin_it != builtins_.end()) {
    return builtin_it->second.factory(num_workers);
  }

  // Plugin creation requires both type and retained DSO registrations.
  auto plugin_it = type_to_plugin_.find(type_name);
  if (plugin_it == type_to_plugin_.end()) {
    return nullptr;
  }

  auto handle_it = loaded_plugins_.find(plugin_it->second);
  if (handle_it == loaded_plugins_.end()) {
    return nullptr;
  }

  const auto& handle = handle_it->second;
  if (!handle.create) {
    return nullptr;
  }

  if (num_workers == 0U || num_workers > kSchedulerWorkerRequestMax) {
    throw std::invalid_argument(
        "scheduler plugin creation requires a resolved worker grant in "
        "[1,8]");
  }

  // Guard the raw ABI result before any host-owner allocation.
  IScheduler* raw_ptr = invoke_scheduler_plugin(handle.library, [&]() {
    return handle.create(type_name.c_str(),
                         static_cast<std::uint32_t>(num_workers));
  });
  if (!raw_ptr) {
    return nullptr;
  }
  RawPluginSchedulerGuard raw_owner(raw_ptr, handle.destroy, handle.library);

  auto owner = std::make_unique<PluginSchedulerOwner>(
      raw_ptr, handle.destroy, handle.library, type_name);
  (void)raw_owner.release();
  return owner;
}

/** @copydoc SchedulerPluginLoader::register_builtin */
void SchedulerPluginLoader::register_builtin(
    const std::string& type_name, const std::string& description,
    std::function<std::unique_ptr<IScheduler>(unsigned int)> factory) {
  std::lock_guard<std::mutex> lock(mutex_);

  BuiltinScheduler builtin;
  builtin.description = description;
  builtin.factory = std::move(factory);
  builtins_[type_name] = std::move(builtin);
}

/** @copydoc SchedulerPluginLoader::clear_plugins */
void SchedulerPluginLoader::clear_plugins() {
  std::lock_guard<std::mutex> lock(mutex_);

  loaded_plugins_.clear();
  type_to_plugin_.clear();
  type_info_.clear();
  // Built-in factories intentionally remain registered.
}

/** @copydoc SchedulerPluginLoader::list_loaded_plugins */
std::vector<std::string> SchedulerPluginLoader::list_loaded_plugins() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;

  for (const auto& [path, handle] : loaded_plugins_) {
    std::string info = path;
    if (!handle.registered_types.empty()) {
      info += " (";
      for (size_t i = 0; i < handle.registered_types.size(); ++i) {
        if (i > 0)
          info += ", ";
        info += handle.registered_types[i];
      }
      info += ")";
    }
    if (!handle.version.empty()) {
      info += " v";
      info += handle.version;
    }
    result.push_back(info);
  }

  return result;
}

/** @copydoc SchedulerPluginLoader::get_load_errors */
std::vector<std::string> SchedulerPluginLoader::get_load_errors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return load_errors_;
}

/** @copydoc SchedulerPluginLoader::clear_errors */
void SchedulerPluginLoader::clear_errors() {
  std::lock_guard<std::mutex> lock(mutex_);
  load_errors_.clear();
}

}  // namespace ps
