#include "compute/compute_request_coordinator.hpp"

#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace ps::compute {

/** @copydoc ComputeRequestCoordinator::PreparedCandidate::PreparedCandidate */
ComputeRequestCoordinator::PreparedCandidate::PreparedCandidate(
    ComputeRequestCoordinator* coordinator,
    SupersessionIdentity identity) noexcept
    : coordinator_(coordinator),         // NOLINT(whitespace/indent_namespace)
      identity_(std::move(identity)) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ComputeRequestCoordinator::PreparedCandidate::PreparedCandidate */
ComputeRequestCoordinator::PreparedCandidate::PreparedCandidate(
    PreparedCandidate&& other) noexcept
    : coordinator_(other.coordinator_),  // NOLINT(whitespace/indent_namespace)
      identity_(other.identity_) {       // NOLINT(whitespace/indent_namespace)
  other.coordinator_ = nullptr;
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ComputeRequestCoordinator::PreparedCandidate::operator= */
ComputeRequestCoordinator::PreparedCandidate&
ComputeRequestCoordinator::PreparedCandidate::operator=(
    PreparedCandidate&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  coordinator_ = std::exchange(other.coordinator_, nullptr);
  identity_ = other.identity_;
  return *this;
}

/** @copydoc ComputeRequestCoordinator::PreparedCandidate::~PreparedCandidate */
ComputeRequestCoordinator::PreparedCandidate::~PreparedCandidate() noexcept {
  reset();
}

/** @copydoc ComputeRequestCoordinator::PreparedCandidate::reset */
void ComputeRequestCoordinator::PreparedCandidate::reset() noexcept {
  if (coordinator_ == nullptr) {
    return;
  }
  try {
    coordinator_->abandon_preparation(identity_);
  } catch (...) {
    std::terminate();
  }
  coordinator_ = nullptr;
}

/** @copydoc ComputeRequestCoordinator::ComputeRequestCoordinator */
ComputeRequestCoordinator::ComputeRequestCoordinator(
    GraphStateExecutor& graph_state, GraphStateExecutor& compute_requests,
    std::uint64_t first_generation)
    : graph_state_(graph_state),
      compute_requests_(compute_requests),
      generation_allocator_(first_generation) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ComputeRequestCoordinator::~ComputeRequestCoordinator */
ComputeRequestCoordinator::~ComputeRequestCoordinator() noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!rows_.empty() || active_key_.has_value()) {
      std::terminate();
    }
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ComputeRequestCoordinator::prepare */
ComputeRequestCoordinator::PreparedCandidate ComputeRequestCoordinator::prepare(
    const SupersessionKey& key) {
  SupersessionIdentity identity{key, SupersessionGeneration(1)};
  bool needs_ticket = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!accepting_) {
      throw std::runtime_error(
          "Compute request supersession admission is closed.");
    }
    identity.generation = generation_allocator_.allocate();
    LineageRow& row = rows_[key];
    if (row.provisional_adopters == std::numeric_limits<std::size_t>::max()) {
      throw std::overflow_error(
          "Compute request provisional adopter count is exhausted.");
    }
    ++row.provisional_adopters;
    while (!row.ticket.valid()) {
      if (!row.reservation_in_progress) {
        row.reservation_in_progress = true;
        needs_ticket = true;
        break;
      }
      reservation_changed_.wait(lock, [this, &key] {
        const auto iterator = rows_.find(key);
        return !accepting_ || iterator == rows_.end() ||
               iterator->second.ticket.valid() ||
               !iterator->second.reservation_in_progress;
      });
      auto iterator = rows_.find(key);
      if (!accepting_ || iterator == rows_.end()) {
        if (iterator != rows_.end()) {
          LineageRow& current = iterator->second;
          if (current.provisional_adopters == 0) {
            throw std::logic_error(
                "Compute request provisional adopter count underflow.");
          }
          --current.provisional_adopters;
          if (current.provisional_adopters == 0 &&
              !current.reservation_in_progress && current.pending == nullptr &&
              current.active == nullptr) {
            rows_.erase(iterator);
          }
        }
        throw std::runtime_error(
            "Compute request supersession admission is closed.");
      }
    }
  }

  if (needs_ticket) {
    GraphStateExecutor::ContinuationTicket reserved_ticket;
    try {
      reserved_ticket = compute_requests_.reserve_continuation(
          [this, key](const GraphStateExecutor::ContinuationTicket& ticket) {
            return pump_turn(key, ticket);
          });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iterator = rows_.find(key);
        if (iterator != rows_.end()) {
          LineageRow& row = iterator->second;
          row.reservation_in_progress = false;
          if (row.provisional_adopters == 0) {
            std::terminate();
          }
          --row.provisional_adopters;
          if (row.provisional_adopters == 0 && row.pending == nullptr &&
              row.active == nullptr) {
            rows_.erase(iterator);
          }
        }
      }
      reservation_changed_.notify_all();
      throw;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto iterator = rows_.find(key);
      if (iterator == rows_.end() ||
          !iterator->second.reservation_in_progress ||
          iterator->second.ticket.valid()) {
        std::terminate();
      }
      iterator->second.ticket = reserved_ticket;
      iterator->second.reservation_in_progress = false;
    }
    reservation_changed_.notify_all();
  }
  return PreparedCandidate(this, std::move(identity));
}

/** @copydoc ComputeRequestCoordinator::publish */
void ComputeRequestCoordinator::publish(
    PreparedCandidate prepared,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation,
    ExecuteCallback execute, SupersededCallback settle_superseded,
    FailureCallback settle_failure) {
  if (prepared.coordinator_ != this || cancellation == nullptr || !execute ||
      !settle_superseded || !settle_failure) {
    throw std::invalid_argument(
        "Compute request publication requires complete prepared ownership.");
  }

  auto candidate = std::make_shared<Candidate>(
      Candidate{prepared.identity_, std::move(cancellation), std::move(execute),
                std::move(settle_superseded), std::move(settle_failure)});
  (void)graph_state_.submit([this, prepared = std::move(prepared),
                             candidate](GraphModel&) mutable {
    try {
      GraphStateExecutor::ContinuationTicket cleanup_ticket;
      GraphStateExecutor::ContinuationTicket published_ticket;
      std::shared_ptr<Candidate> displaced_pending;
      std::shared_ptr<ComputeRequestCancellationSource> displaced_active;
      bool born_superseded = false;
      bool rejected_by_close = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto row_iterator = rows_.find(prepared.identity_.key);
        if (row_iterator == rows_.end() ||
            row_iterator->second.provisional_adopters == 0) {
          throw std::logic_error(
              "Supersession publication lost provisional ownership.");
        }
        LineageRow& row = row_iterator->second;
        --row.provisional_adopters;

        rejected_by_close = !accepting_ || !row.ticket.valid();
        if (!rejected_by_close && row.current_generation.has_value() &&
            !(row.current_generation.value() < prepared.identity_.generation)) {
          born_superseded = true;
        }

        if (!rejected_by_close && !born_superseded) {
          row.current_generation = prepared.identity_.generation;
          displaced_pending = std::move(row.pending);
          if (row.active != nullptr) {
            displaced_active = row.active->cancellation;
          }
          row.pending = candidate;
          published_ticket = row.ticket;
        } else if (row.provisional_adopters == 0 && row.pending == nullptr &&
                   row.active == nullptr && !row.reservation_in_progress) {
          cleanup_ticket = row.ticket;
          rows_.erase(row_iterator);
        }
        prepared.coordinator_ = nullptr;
      }

      if (cleanup_ticket.valid()) {
        (void)cleanup_ticket.retire();
      }
      if (rejected_by_close) {
        try {
          throw std::runtime_error(
              "Compute request publication rejected by Graph close.");
        } catch (...) {
          settle_failure_noexcept(candidate, std::current_exception());
        }
        return;
      }
      if (born_superseded) {
        request_superseded_noexcept(candidate->cancellation);
        settle_superseded_noexcept(candidate);
        return;
      }

      request_superseded_noexcept(displaced_active);
      if (displaced_pending != nullptr) {
        request_superseded_noexcept(displaced_pending->cancellation);
        settle_superseded_noexcept(displaced_pending);
      }
      (void)published_ticket.wake();
    } catch (...) {
      settle_failure_noexcept(candidate, std::current_exception());
    }
  });
}

/** @copydoc ComputeRequestCoordinator::pump_turn */
GraphStateExecutor::ContinuationAction ComputeRequestCoordinator::pump_turn(
    const SupersessionKey& key,
    const GraphStateExecutor::ContinuationTicket& ticket) noexcept {
  std::shared_ptr<Candidate> candidate;
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto row_iterator = rows_.find(key);
      if (row_iterator == rows_.end() ||
          !row_iterator->second.ticket.same_reservation(ticket)) {
        return GraphStateExecutor::ContinuationAction::Retire;
      }
      LineageRow& row = row_iterator->second;
      if (row.pending == nullptr) {
        if (row.active == nullptr && row.provisional_adopters == 0) {
          row.ticket = GraphStateExecutor::ContinuationTicket();
          rows_.erase(row_iterator);
          return GraphStateExecutor::ContinuationAction::Retire;
        }
        return GraphStateExecutor::ContinuationAction::Park;
      }
      if (active_key_.has_value()) {
        return GraphStateExecutor::ContinuationAction::Queue;
      }
      candidate = std::move(row.pending);
      row.active = candidate;
      active_key_ = key;
    }

    try {
      candidate->execute();
    } catch (...) {
      try {
        candidate->settle_failure(std::current_exception());
      } catch (...) {
      }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto row_iterator = rows_.find(key);
    if (row_iterator == rows_.end() ||
        !row_iterator->second.ticket.same_reservation(ticket) ||
        row_iterator->second.active != candidate || !active_key_.has_value() ||
        !(*active_key_ == key)) {
      return GraphStateExecutor::ContinuationAction::Retire;
    }
    LineageRow& row = row_iterator->second;
    row.active.reset();
    active_key_.reset();
    if (row.pending != nullptr) {
      return GraphStateExecutor::ContinuationAction::Queue;
    }
    if (row.provisional_adopters != 0) {
      return GraphStateExecutor::ContinuationAction::Park;
    }
    row.ticket = GraphStateExecutor::ContinuationTicket();
    rows_.erase(row_iterator);
    return GraphStateExecutor::ContinuationAction::Retire;
  } catch (...) {
    if (candidate != nullptr) {
      try {
        candidate->settle_failure(std::current_exception());
      } catch (...) {
      }
    }
    return GraphStateExecutor::ContinuationAction::Retire;
  }
}

/** @copydoc ComputeRequestCoordinator::abandon_preparation */
void ComputeRequestCoordinator::abandon_preparation(
    const SupersessionIdentity& identity) {
  GraphStateExecutor::ContinuationTicket cleanup_ticket;
  GraphStateExecutor::ContinuationTicket resume_ticket;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto row_iterator = rows_.find(identity.key);
    if (row_iterator != rows_.end()) {
      LineageRow& row = row_iterator->second;
      if (row.provisional_adopters == 0) {
        throw std::logic_error(
            "Supersession preparation adopter count underflow.");
      }
      --row.provisional_adopters;
      if (row.provisional_adopters == 0 && row.pending == nullptr &&
          row.active == nullptr && !row.reservation_in_progress) {
        if (row.ticket.valid()) {
          cleanup_ticket = row.ticket;
        }
        rows_.erase(row_iterator);
      } else if (row.ticket.valid()) {
        resume_ticket = row.ticket;
      }
    }
  }
  if (cleanup_ticket.valid()) {
    (void)cleanup_ticket.retire();
  }
  if (resume_ticket.valid()) {
    (void)resume_ticket.resume_after_settlement();
  }
}

/** @copydoc ComputeRequestCoordinator::request_superseded_noexcept */
void ComputeRequestCoordinator::request_superseded_noexcept(
    const std::shared_ptr<ComputeRequestCancellationSource>& source) noexcept {
  if (source == nullptr) {
    return;
  }
  try {
    (void)source->request_cancellation(
        ComputeRunCancellationReason::Superseded);
  } catch (...) {
  }
}

/** @copydoc ComputeRequestCoordinator::settle_superseded_noexcept */
void ComputeRequestCoordinator::settle_superseded_noexcept(
    const std::shared_ptr<Candidate>& candidate) noexcept {
  if (candidate == nullptr) {
    return;
  }
  try {
    candidate->settle_superseded();
  } catch (...) {
  }
}

/** @copydoc ComputeRequestCoordinator::settle_failure_noexcept */
void ComputeRequestCoordinator::settle_failure_noexcept(
    const std::shared_ptr<Candidate>& candidate,
    std::exception_ptr failure) noexcept {
  if (candidate == nullptr) {
    return;
  }
  try {
    candidate->settle_failure(std::move(failure));
  } catch (...) {
  }
}

/** @copydoc ComputeRequestCoordinator::is_current */
bool ComputeRequestCoordinator::is_current(
    const SupersessionIdentity& identity) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto row_iterator = rows_.find(identity.key);
  return row_iterator != rows_.end() &&
         row_iterator->second.current_generation.has_value() &&
         row_iterator->second.current_generation.value() == identity.generation;
}

/** @copydoc ComputeRequestCoordinator::stop_admission */
void ComputeRequestCoordinator::stop_admission() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
  }
  reservation_changed_.notify_all();
}

/** @copydoc ComputeRequestCoordinator::snapshot */
ComputeRequestCoordinator::Snapshot ComputeRequestCoordinator::snapshot()
    const {
  Snapshot result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result.lineage_rows = rows_.size();
    for (const auto& [key, row] : rows_) {
      (void)key;
      result.reserved_tickets += row.ticket.valid() ? 1U : 0U;
      result.pending_candidates += row.pending != nullptr ? 1U : 0U;
      result.active_candidates += row.active != nullptr ? 1U : 0U;
      result.provisional_adopters += row.provisional_adopters;
    }
  }
  result.lane_admitted_units = compute_requests_.admitted_units();
  return result;
}

}  // namespace ps::compute
