#include "compute/realtime_proxy_graph.hpp"

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ps::compute {

/** @copydoc RealtimeProxyGraph::clone_for_compute */
std::unique_ptr<RealtimeProxyGraph> RealtimeProxyGraph::clone_for_compute()
    const {  // NOLINT(whitespace/indent_namespace)
  auto snapshot = std::make_unique<RealtimeProxyGraph>();
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot->nodes_ = nodes_;
  snapshot->topology_generation_ = topology_generation_;
  return snapshot;
}

/** @copydoc RealtimeProxyGraph::publish_compute_snapshot */
void RealtimeProxyGraph::publish_compute_snapshot(
    RealtimeProxyGraph& prepared) noexcept {
  std::scoped_lock lock(mutex_, prepared.mutex_);
  nodes_.swap(prepared.nodes_);
  std::swap(topology_generation_, prepared.topology_generation_);
}

void RealtimeProxyGraph::synchronize_with_graph(const GraphModel& graph) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::vector<int> ids = graph.node_ids();
  const uint64_t graph_generation = graph.topology_generation();
  if (topology_generation_ != graph_generation) {
    nodes_.clear();
    for (int id : ids) {
      nodes_.emplace(id, NodeState{});
    }
    topology_generation_ = graph_generation;
    return;
  }
  std::set<int> live_ids(ids.begin(), ids.end());
  for (auto it = nodes_.begin(); it != nodes_.end();) {
    if (!live_ids.count(it->first)) {
      it = nodes_.erase(it);
      continue;
    }
    ++it;
  }
  for (int id : ids) {
    nodes_.try_emplace(id);
  }
  topology_generation_ = graph_generation;
}

uint64_t RealtimeProxyGraph::topology_generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return topology_generation_;
}

void RealtimeProxyGraph::reset_nodes(const std::vector<int>& node_ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int node_id : node_ids) {
    nodes_[node_id] = NodeState{};
  }
}

const NodeOutput* RealtimeProxyGraph::find_output(int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end() || !it->second.output) {
    return nullptr;
  }
  return &*it->second.output;
}

const RealtimeProxyGraph::NodeState* RealtimeProxyGraph::find_state(
    int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    return nullptr;
  }
  return &it->second;
}

uint64_t RealtimeProxyGraph::dirty_source_generation(int node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end() || !it->second.dirty_source_generation) {
    return 0;
  }
  return *it->second.dirty_source_generation;
}

void RealtimeProxyGraph::commit_node_state(int node_id, NodeState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  nodes_[node_id] = std::move(state);
}

NodeOutput& RealtimeProxyGraph::require_output(int node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end() || !it->second.output) {
    throw GraphError(GraphErrc::ComputeError,
                     "RT compute finished without proxy target output.");
  }
  return *it->second.output;
}

}  // namespace ps::compute
