#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/core/export.hpp"

namespace ps {

/**
 * @brief Closed scalar parameter value used by WorkflowDocument.
 *
 * @note Parameters are source values and never store runtime pointers.
 */
using ParameterValue = std::variant<std::int64_t, double, bool, std::string>;

/**
 * @brief One directed input edge from a producer node.
 *
 * @note The current compiler models one output Value per node; `source_port`
 * remains explicit for document evolution and must currently be `value`.
 */
struct PHOTOSPIDER_API WorkflowInput final {
  /** @brief Nonzero producer node id. */
  std::uint64_t source_node = 0;
  /** @brief Producer output port, currently `value`. */
  std::string source_port = "value";
};

/**
 * @brief One operation invocation in a WorkflowDocument.
 *
 * @note Node order is source order only; semantic order is derived from edges.
 */
struct PHOTOSPIDER_API WorkflowNode final {
  /** @brief Nonzero document-local node id. */
  std::uint64_t id = 0;
  /** @brief Stable registered operation key. */
  std::string operation;
  /** @brief Ordered input edges matching the operation descriptor. */
  std::vector<WorkflowInput> inputs;
  /** @brief Canonically ordered parameter map. */
  std::map<std::string, ParameterValue> parameters;
};

/**
 * @brief One named workflow output selection.
 *
 * @note Output names are caller-visible labels and must be unique.
 */
struct PHOTOSPIDER_API WorkflowOutput final {
  /** @brief Nonempty caller-visible output name. */
  std::string name;
  /** @brief Nonzero source node id. */
  std::uint64_t node_id = 0;
  /** @brief Selected node output port, currently `value`. */
  std::string port = "value";
};

/**
 * @brief Format-neutral caller-owned source graph.
 *
 * @note A WorkflowDocument is compiler input, not a storage service or daemon
 * identity.
 */
struct PHOTOSPIDER_API WorkflowDocument final {
  /** @brief Positive source schema version; current writer emits 1. */
  std::uint32_t schema_version = 1;
  /** @brief Source nodes; semantic ordering is derived during analysis. */
  std::vector<WorkflowNode> nodes;
  /** @brief Named values requested from successful execution. */
  std::vector<WorkflowOutput> outputs;
};

/**
 * @brief Immutable snapshot of one GraphContext revision.
 *
 * @note Snapshot copies may outlive replacement. `current()` detects whether
 * publication remains legal for the captured revision.
 */
class PHOTOSPIDER_API GraphSnapshot final {
 public:
  /**
   * @brief Constructs an empty invalid/default snapshot.
   * @throws Nothing.
   * @note Default snapshots are rejected by the compiler.
   */
  GraphSnapshot() noexcept = default;

  /**
   * @brief Returns the captured source document.
   * @return Immutable source document reference.
   * @throws std::logic_error If this snapshot is invalid/default.
   * @note Reference lifetime is bounded by this snapshot.
   */
  [[nodiscard]] const WorkflowDocument& document() const;

  /**
   * @brief Returns the captured graph revision.
   * @return Nonzero monotonic revision.
   * @throws std::logic_error If this snapshot is invalid/default.
   * @note Revisions are local to the owning GraphContext.
   */
  [[nodiscard]] std::uint64_t revision() const;

  /**
   * @brief Reports whether this snapshot can still publish.
   * @return True only while the owning context is alive at this revision.
   * @throws Nothing.
   * @note Destruction or replacement makes the result false monotonically.
   */
  [[nodiscard]] bool current() const noexcept;

 private:
  friend class GraphContext;

  /** @brief Shared revision state implemented in the source module. */
  struct State;

  /**
   * @brief Captures a document/revision and weak currentness state.
   * @param document Immutable copied source document.
   * @param revision Nonzero captured revision.
   * @param state Weak reference to the graph revision state.
   * @throws std::bad_alloc If shared document storage allocation fails.
   * @note Only GraphContext constructs valid snapshots.
   */
  GraphSnapshot(std::shared_ptr<const WorkflowDocument> document,
                std::uint64_t revision, std::weak_ptr<State> state);

  /** @brief Shared immutable source document. */
  std::shared_ptr<const WorkflowDocument> document_;
  /** @brief Nonzero captured revision, or zero for default state. */
  std::uint64_t revision_ = 0;
  /** @brief Weak liveness/currentness state of the owning graph context. */
  std::weak_ptr<State> state_;
};

/**
 * @brief Independently owned kernel graph and monotonic source revision.
 *
 * @note A GraphContext is not a daemon Session and is never registered in a
 * kernel-global namespace.
 */
class PHOTOSPIDER_API GraphContext final {
 public:
  /**
   * @brief Creates a context at revision one.
   * @param document Initial caller-owned source document copied into context.
   * @throws std::bad_alloc If source/revision state cannot be allocated.
   * @note Semantic validation occurs when the compiler analyzes a snapshot.
   */
  explicit GraphContext(WorkflowDocument document);

  /**
   * @brief Destroys the graph and invalidates every outstanding snapshot.
   * @throws Nothing.
   * @note Shared ExecutionContext objects are not stopped by graph destruction.
   */
  ~GraphContext() noexcept;

  /**
   * @brief Forbids copying graph revision/currentness ownership.
   * @param other Source context that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Construct a new context from an explicit document snapshot instead.
   */
  GraphContext(const GraphContext& other) = delete;
  /**
   * @brief Forbids copy assignment across independent graph identities.
   * @param other Source context that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Existing snapshot currentness is never rebound.
   */
  GraphContext& operator=(const GraphContext& other) = delete;
  /**
   * @brief Forbids moving a context whose address-independent state is shared.
   * @param other Source context that cannot be moved.
   * @throws Nothing; the operation is deleted.
   * @note Stable lifetime keeps outstanding snapshot checks deterministic.
   */
  GraphContext(GraphContext&& other) = delete;
  /**
   * @brief Forbids move assignment of graph identity and revision state.
   * @param other Source context that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Replace the document explicitly through `replace()` instead.
   */
  GraphContext& operator=(GraphContext&& other) = delete;

  /**
   * @brief Captures a coherent immutable source/revision pair.
   * @return Snapshot valid until replacement or context destruction.
   * @throws std::bad_alloc If document copy allocation fails.
   * @note Snapshot capture is serialized with replacement.
   */
  [[nodiscard]] GraphSnapshot snapshot() const;

  /**
   * @brief Replaces source state and advances the monotonic revision.
   * @param document New source document copied before publication.
   * @return Newly published nonzero revision.
   * @throws std::overflow_error If the revision would overflow.
   * @throws std::bad_alloc If source copy allocation fails; state is unchanged.
   * @note Existing compiled plans become stale immediately after publication.
   */
  std::uint64_t replace(WorkflowDocument document);

  /**
   * @brief Returns the current monotonic revision.
   * @return Nonzero revision.
   * @throws Nothing.
   * @note The observation may become stale immediately after return.
   */
  [[nodiscard]] std::uint64_t revision() const noexcept;

 private:
  /** @brief Shared currentness state retained weakly by snapshots. */
  std::shared_ptr<GraphSnapshot::State> state_;
  /** @brief Serializes document/revision capture and replacement. */
  mutable std::mutex mutex_;
  /** @brief Current immutable source document. */
  std::shared_ptr<const WorkflowDocument> document_;
};

}  // namespace ps
