#include "ipc/collection_snapshot_registry.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "ipc/codec.hpp"

namespace ps::ipc::internal {

/** @copydoc CollectionSnapshotRegistry::Reservation::Reservation */
CollectionSnapshotRegistry::Reservation::Reservation(
    CollectionSnapshotRegistry* owner, std::size_t token) noexcept
    : owner_(owner), token_(token) {}  // NOLINT

/** @copydoc CollectionSnapshotRegistry::Reservation::~Reservation */
CollectionSnapshotRegistry::Reservation::~Reservation() noexcept {
  CollectionSnapshotRegistry* owner = std::exchange(owner_, nullptr);
  if (owner != nullptr) {
    owner->cancel_reservation(token_);
  }
  token_ = 0;
}

/** @copydoc CollectionSnapshotRegistry::Reservation::Reservation */
CollectionSnapshotRegistry::Reservation::Reservation(
    Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),  // NOLINT
      token_(std::exchange(other.token_, 0)) {}      // NOLINT

/** @copydoc CollectionSnapshotRegistry::Reservation::operator= */
CollectionSnapshotRegistry::Reservation&
CollectionSnapshotRegistry::Reservation::operator=(
    Reservation&& other) noexcept {
  if (this != &other) {
    CollectionSnapshotRegistry* owner = std::exchange(owner_, nullptr);
    if (owner != nullptr) {
      owner->cancel_reservation(token_);
    }
    owner_ = std::exchange(other.owner_, nullptr);
    token_ = std::exchange(other.token_, 0);
  }
  return *this;
}

/** @copydoc CollectionSnapshotRegistry::Reservation::active */
bool CollectionSnapshotRegistry::Reservation::active() const noexcept {
  return owner_ != nullptr && owner_->reservation_active(token_);
}

/** @copydoc CollectionSnapshotRegistry::CollectionSnapshotRegistry */
CollectionSnapshotRegistry::CollectionSnapshotRegistry(
    CollectionSnapshotLimits limits, Clock clock, IdGenerator id_generator)
    : limits_(limits),
      clock_(std::move(clock)),
      id_generator_(std::move(id_generator)) {
  if (limits_.records == 0 || limits_.total_bytes == 0 ||
      limits_.reservation_bytes == 0 || limits_.snapshot_entries == 0 ||
      limits_.snapshot_bytes == 0 || limits_.page_entries == 0 ||
      limits_.ttl <= std::chrono::steady_clock::duration::zero() ||
      limits_.reservation_bytes < limits_.snapshot_bytes ||
      limits_.total_bytes < limits_.reservation_bytes) {
    throw std::invalid_argument("invalid collection snapshot registry limits");
  }
  if (!clock_) {
    clock_ = [] { return std::chrono::steady_clock::now(); };
  }
  if (!id_generator_) {
    id_generator_ = generate_opaque_id;
  }
}

/** @copydoc CollectionSnapshotRegistry::start */
void CollectionSnapshotRegistry::start() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = true;
}

/** @copydoc CollectionSnapshotRegistry::reserve */
CollectionSnapshotRegistry::ReserveResult
CollectionSnapshotRegistry::reserve() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = clock_();
  expire_locked(now);
  if (!accepting_) {
    return {CollectionSnapshotError::Stopped, {}};
  }
  if (records_.size() >= limits_.records ||
      reservations_.size() >= limits_.records - records_.size()) {
    return {CollectionSnapshotError::CapacityExceeded, {}};
  }
  if (retained_bytes_ > limits_.total_bytes ||
      reserved_bytes_ > limits_.total_bytes - retained_bytes_ ||
      limits_.reservation_bytes >
          limits_.total_bytes - retained_bytes_ - reserved_bytes_) {
    return {CollectionSnapshotError::CapacityExceeded, {}};
  }

  std::size_t token = 0;
  for (std::size_t attempts = 0; attempts <= limits_.records; ++attempts) {
    token = next_reservation_token_++;
    if (next_reservation_token_ == 0) {
      next_reservation_token_ = 1;
    }
    if (token == 0) {
      continue;
    }
    if (reservations_.emplace(token, true).second) {
      break;
    }
    token = 0;
  }
  if (token == 0) {
    throw std::runtime_error("collection reservation token space exhausted");
  }
  reserved_bytes_ += limits_.reservation_bytes;
  return {CollectionSnapshotError::None, Reservation(this, token)};
}

/** @copydoc CollectionSnapshotRegistry::begin_shutdown */
void CollectionSnapshotRegistry::begin_shutdown() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = false;
}

/** @copydoc CollectionSnapshotRegistry::finish_shutdown */
void CollectionSnapshotRegistry::finish_shutdown() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = false;
  records_.clear();
  reservations_.clear();
  retained_bytes_ = 0;
  reserved_bytes_ = 0;
}

/** @copydoc CollectionSnapshotRegistry::record_count */
std::size_t CollectionSnapshotRegistry::record_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

/** @copydoc CollectionSnapshotRegistry::retained_bytes */
std::size_t CollectionSnapshotRegistry::retained_bytes() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return retained_bytes_;
}

/** @copydoc CollectionSnapshotRegistry::consume_reservation_locked */
bool CollectionSnapshotRegistry::consume_reservation_locked(
    Reservation* reservation, std::size_t retained_bytes) noexcept {
  if (reservation == nullptr || reservation->owner_ != this ||
      retained_bytes > limits_.reservation_bytes ||
      retained_bytes_ > limits_.total_bytes - retained_bytes) {
    return false;
  }
  const auto found = reservations_.find(reservation->token_);
  if (found == reservations_.end() ||
      reserved_bytes_ < limits_.reservation_bytes) {
    return false;
  }

  reservations_.erase(found);
  reserved_bytes_ -= limits_.reservation_bytes;
  retained_bytes_ += retained_bytes;
  reservation->owner_ = nullptr;
  reservation->token_ = 0;
  return true;
}

/** @copydoc CollectionSnapshotRegistry::cancel_reservation */
void CollectionSnapshotRegistry::cancel_reservation(
    std::size_t token) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = reservations_.find(token);
  if (found == reservations_.end()) {
    return;
  }
  reservations_.erase(found);
  if (reserved_bytes_ >= limits_.reservation_bytes) {
    reserved_bytes_ -= limits_.reservation_bytes;
  } else {
    reserved_bytes_ = 0;
  }
}

/** @copydoc CollectionSnapshotRegistry::reservation_active */
bool CollectionSnapshotRegistry::reservation_active(
    std::size_t token) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return reservations_.count(token) != 0;
}

/** @copydoc CollectionSnapshotRegistry::valid_cursor */
bool CollectionSnapshotRegistry::valid_cursor(
    std::string_view cursor) noexcept {
  return valid_opaque_id(cursor);
}

/** @copydoc CollectionSnapshotRegistry::expire_locked */
void CollectionSnapshotRegistry::expire_locked(
    std::chrono::steady_clock::time_point now) noexcept {
  auto current = records_.begin();
  while (current != records_.end()) {
    if (now >= current->second.expires_at) {
      current = erase_locked(current);
    } else {
      ++current;
    }
  }
}

/** @copydoc CollectionSnapshotRegistry::expiration_at */
std::chrono::steady_clock::time_point CollectionSnapshotRegistry::expiration_at(
    std::chrono::steady_clock::time_point now) const noexcept {
  const auto latest_start =
      std::chrono::steady_clock::time_point::max() - limits_.ttl;
  if (now > latest_start) {
    return std::chrono::steady_clock::time_point::max();
  }
  return now + limits_.ttl;
}

/** @copydoc CollectionSnapshotRegistry::erase_locked */
std::map<std::string, CollectionSnapshotRegistry::Record>::iterator
CollectionSnapshotRegistry::erase_locked(
    std::map<std::string, Record>::iterator iterator) noexcept {
  if (retained_bytes_ >= iterator->second.measured_bytes) {
    retained_bytes_ -= iterator->second.measured_bytes;
  } else {
    retained_bytes_ = 0;
  }
  return records_.erase(iterator);
}

}  // namespace ps::ipc::internal
