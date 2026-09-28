#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ps::execution_internal {
/**
 * @brief ExecutionContext-wide nonblocking waiting-callback admission owner.
 *
 * Every acquisition represents an admitted callback or external stage job.
 * Ordinary callbacks release their lease on worker start; external stages
 * retain it through drain and unlink. Move-only leases also release exactly
 * once on enqueue rollback, exception unwinding, or queue drop.
 */
class WaitingAdmission final {
 public:
  /**
   * @brief Move-only exact-release token for one waiting callback.
   *
   * @note A default or moved-from token owns no admission and is safe to drop.
   */
  class Lease final {
   public:
    /**
     * @brief Constructs an empty non-owning token.
     * @throws Nothing.
     */
    Lease() noexcept = default;

    /**
     * @brief Transfers one waiting-callback admission.
     * @param other Source token left empty.
     * @throws Nothing.
     */
    Lease(Lease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)) {}

    /**
     * @brief Releases current ownership before accepting another token.
     * @param other Source token left empty.
     * @return This token.
     * @throws Nothing.
     */
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
      }
      return *this;
    }

    /**
     * @brief Releases the owned waiting admission exactly once.
     * @throws Nothing under the ExecutionContext lifetime contract.
     */
    ~Lease() noexcept { release(); }

    /**
     * @brief Forbids duplicating exact-release ownership.
     * @param other Source token that cannot be copied.
     * @throws Nothing; the operation is deleted.
     */
    Lease(const Lease& other) = delete;
    /**
     * @brief Forbids assigning duplicate exact-release ownership.
     * @param other Source token that cannot be assigned.
     * @return No value; the operation is deleted.
     * @throws Nothing; the operation is deleted.
     */
    Lease& operator=(const Lease& other) = delete;

    /**
     * @brief Releases ownership early when a worker begins the callback.
     * @return No value.
     * @throws Nothing under the ExecutionContext lifetime contract.
     * @note Repeated calls are idempotent.
     */
    void release() noexcept {
      if (owner_) {
        owner_->release();
        owner_ = nullptr;
      }
    }

   private:
    friend class WaitingAdmission;

    /**
     * @brief Constructs one owning token.
     * @param owner Context-wide admission owner.
     * @throws Nothing.
     */
    explicit Lease(WaitingAdmission* owner) noexcept : owner_(owner) {}

    /** @brief Admission owner receiving release, or null. */
    WaitingAdmission* owner_ = nullptr;
  };

  /**
   * @brief Constructs a positive shared waiting-callback limit.
   * @param capacity Maximum aggregate callbacks waiting across all lanes.
   * @throws std::invalid_argument If capacity is zero.
   */
  explicit WaitingAdmission(std::uint32_t capacity) : capacity_(capacity) {
    if (capacity == 0U) {
      throw std::invalid_argument("maximum queued tasks must be positive");
    }
  }

  /**
   * @brief Attempts immediate aggregate waiting admission.
   * @return Owning token, or empty when the context-wide limit is full.
   * @throws std::system_error If mutex acquisition fails.
   * @note The comparison precedes increment, preventing unsigned overflow.
   */
  [[nodiscard]] std::optional<Lease> try_acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting_ >= capacity_) {
      return std::nullopt;
    }
    ++waiting_;
    return Lease(this);
  }

 private:
  /**
   * @brief Releases one previously acquired waiting admission.
   * @return No value.
   * @throws Nothing under the move-only token invariant.
   */
  void release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting_ != 0U) {
      --waiting_;
    }
  }

  /** @brief Serializes aggregate waiting count. */
  std::mutex mutex_;
  /** @brief Fixed positive aggregate limit. */
  const std::size_t capacity_;
  /** @brief Callbacks currently queued but not started. */
  std::size_t waiting_ = 0U;
};

}  // namespace ps::execution_internal
