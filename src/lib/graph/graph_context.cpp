#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "photospider/compiler/workflow_document.hpp"

namespace ps {

/**
 * @brief Shared liveness and current revision observed by graph snapshots.
 *
 * @note The atomic revision becomes zero exactly once during context teardown.
 */
struct GraphSnapshot::State final {
  /** @brief Current nonzero revision, or zero after destruction. */
  std::atomic<std::uint64_t> revision{1U};
};

/**
 * @brief Implements private coherent graph snapshot capture.
 * @copydetails GraphSnapshot::GraphSnapshot
 */
GraphSnapshot::GraphSnapshot(std::shared_ptr<const WorkflowDocument> document,
                             std::uint64_t revision, std::weak_ptr<State> state)
    : document_(std::move(document)), revision_(revision) {
  state_ = std::move(state);
}

/**
 * @brief Implements captured source-document access.
 * @copydetails GraphSnapshot::document
 */
const WorkflowDocument& GraphSnapshot::document() const {
  if (!document_ || revision_ == 0U) {
    throw std::logic_error("default GraphSnapshot has no document");
  }
  return *document_;
}

/**
 * @brief Implements captured revision access.
 * @copydetails GraphSnapshot::revision
 */
std::uint64_t GraphSnapshot::revision() const {
  if (!document_ || revision_ == 0U) {
    throw std::logic_error("default GraphSnapshot has no revision");
  }
  return revision_;
}

/**
 * @brief Implements monotonic snapshot-currentness observation.
 * @copydetails GraphSnapshot::current
 */
bool GraphSnapshot::current() const noexcept {
  const std::shared_ptr<State> state = state_.lock();
  return state && state->revision.load(std::memory_order_acquire) == revision_;
}

/**
 * @brief Implements revision-one source context construction.
 * @copydetails GraphContext::GraphContext
 */
GraphContext::GraphContext(WorkflowDocument document)
    : state_(std::make_shared<GraphSnapshot::State>()),
      document_(std::make_shared<const WorkflowDocument>(std::move(document))) {
}

/**
 * @brief Implements snapshot invalidation during context teardown.
 * @copydetails GraphContext::~GraphContext
 */
GraphContext::~GraphContext() noexcept {
  state_->revision.store(0U, std::memory_order_release);
}

/**
 * @brief Implements coherent source/revision snapshot capture.
 * @copydetails GraphContext::snapshot
 */
GraphSnapshot GraphContext::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return GraphSnapshot(
      document_, state_->revision.load(std::memory_order_acquire), state_);
}

/**
 * @brief Implements atomic source replacement and revision advance.
 * @copydetails GraphContext::replace
 */
std::uint64_t GraphContext::replace(WorkflowDocument document) {
  auto replacement =
      std::make_shared<const WorkflowDocument>(std::move(document));
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t current =
      state_->revision.load(std::memory_order_relaxed);
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("GraphContext revision overflow");
  }
  const std::uint64_t next = current + 1U;
  document_ = std::move(replacement);
  state_->revision.store(next, std::memory_order_release);
  return next;
}

/**
 * @brief Implements current revision observation.
 * @copydetails GraphContext::revision
 */
std::uint64_t GraphContext::revision() const noexcept {
  return state_->revision.load(std::memory_order_acquire);
}

}  // namespace ps
