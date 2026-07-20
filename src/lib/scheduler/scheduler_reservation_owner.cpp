/**
 * @file scheduler_reservation_owner.cpp
 * @brief Implements transparent scheduler/reservation lifetime composition.
 */

#include "scheduler/scheduler_reservation_owner.hpp"

#include <cassert>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Transparently owns one concrete scheduler and its ledger admission.
 *
 * Every lifecycle and task-runtime call delegates directly to `scheduler_`.
 * The destructor explicitly destroys that inner owner first; only after the
 * destructor body does member destruction release `reservation_`.
 *
 * @throws Only exceptions produced by the forwarded concrete scheduler calls.
 * @note Lifecycle cleanup remains `GraphRuntime`'s responsibility. This owner
 * does not retry, suppress, or normalize shutdown/detach exceptions.
 */
class ReservationOwnedScheduler final : public IScheduler {
 public:
  /**
   * @brief Takes one active reservation and non-null concrete scheduler.
   * @param reservation Exact admitted slots retained for the scheduler.
   * @param scheduler Concrete scheduler delegated until destruction.
   * @throws Nothing; both owners move without allocation.
   */
  ReservationOwnedScheduler(ResourceLedger::Reservation reservation,
                            std::unique_ptr<IScheduler> scheduler) noexcept
      : reservation_(std::move(reservation)), scheduler_(std::move(scheduler)) {
    assert(reservation_.active());
    assert(scheduler_ != nullptr);
  }

  /**
   * @brief Destroys the concrete scheduler before reservation member teardown.
   * @throws Nothing under the concrete `IScheduler` destructor contract.
   * @note Explicit reset guards the ordering invariant against future member
   * reordering; `reservation_` releases only after this body finishes.
   */
  ~ReservationOwnedScheduler() noexcept override { scheduler_.reset(); }

  /**
   * @brief Prevents duplicate scheduler and reservation ownership.
   * @param other Owner that remains the sole owner.
   * @throws Nothing because the operation is deleted.
   */
  ReservationOwnedScheduler(const ReservationOwnedScheduler& other) = delete;

  /**
   * @brief Prevents replacement through duplicated owner state.
   * @param other Owner that remains unchanged.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ReservationOwnedScheduler& operator=(const ReservationOwnedScheduler& other) =
      delete;

  /** @copydoc IScheduler::attach */
  void attach(SchedulerHostContext& host) override { scheduler_->attach(host); }

  /** @copydoc IScheduler::detach */
  void detach() override { scheduler_->detach(); }

  /** @copydoc IScheduler::start */
  void start() override { scheduler_->start(); }

  /** @copydoc IScheduler::shutdown */
  void shutdown() override { scheduler_->shutdown(); }

  /** @copydoc IScheduler::name */
  std::string name() const override { return scheduler_->name(); }

  /** @copydoc IScheduler::get_stats */
  std::string get_stats() const override { return scheduler_->get_stats(); }

  /** @copydoc IScheduler::is_running */
  bool is_running() const override { return scheduler_->is_running(); }

  /** @copydoc SchedulerTaskRuntime::available_devices */
  std::vector<Device> available_devices() const override {
    return scheduler_->available_devices();
  }

  /** @copydoc SchedulerTaskRuntime::submit_initial_task_handles */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    scheduler_->submit_initial_task_handles(std::move(handles),
                                            total_task_count, priority);
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_handles_from_worker */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    scheduler_->submit_ready_task_handles_from_worker(std::move(handles),
                                                      priority);
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_any_thread */
  void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) override {
    scheduler_->submit_ready_task_any_thread(std::move(task), priority, epoch);
  }

  /** @copydoc SchedulerTaskRuntime::wait_for_completion */
  void wait_for_completion() override { scheduler_->wait_for_completion(); }

  /** @copydoc SchedulerTaskRuntime::set_exception */
  void set_exception(std::exception_ptr error) override {
    scheduler_->set_exception(error);
  }

  /** @copydoc SchedulerTaskRuntime::inc_tasks_to_complete */
  void inc_tasks_to_complete(int delta) override {
    scheduler_->inc_tasks_to_complete(delta);
  }

  /** @copydoc SchedulerTaskRuntime::dec_tasks_to_complete */
  void dec_tasks_to_complete() override { scheduler_->dec_tasks_to_complete(); }

  /** @copydoc SchedulerTaskRuntime::log_event */
  void log_event(SchedulerTraceAction action, int node_id) override {
    scheduler_->log_event(action, node_id);
  }

 private:
  /**
   * @brief Process admission released after the concrete scheduler member.
   * @note Declared first so reverse member destruction reinforces the explicit
   * scheduler reset in the destructor body.
   */
  ResourceLedger::Reservation reservation_;

  /** @brief Concrete scheduler destroyed before `reservation_` releases. */
  std::unique_ptr<IScheduler> scheduler_;
};

}  // namespace

/** @copydoc make_reservation_owned_scheduler */
std::unique_ptr<IScheduler> make_reservation_owned_scheduler(
    std::unique_ptr<IScheduler> scheduler,
    ResourceLedger::Reservation reservation) {
  if (scheduler == nullptr) {
    return nullptr;
  }
  if (!reservation.active()) {
    throw std::invalid_argument(
        "scheduler reservation owner requires an active reservation");
  }

  try {
    return std::unique_ptr<IScheduler>(new ReservationOwnedScheduler(
        std::move(reservation), std::move(scheduler)));
  } catch (...) {
    scheduler.reset();
    throw;
  }
}

}  // namespace ps
