// NodeGraph compute event streaming
//
// Frontends (CLI) poll for compute events while a compute is running. Events are pushed from
// both sequential and parallel paths. This file provides a small, thread-safe, swap-based
// queue: push under a mutex; drain swaps into a local vector and releases the lock quickly.
#include "node_graph.hpp"
#include <utility>

namespace ps {

void NodeGraph::push_compute_event(int id, const std::string& name, const std::string& source, double ms) {
    std::lock_guard<std::mutex> lk(event_mutex_);
    event_buffer_.push_back(ComputeEvent{ id, name, source, ms });
}

std::vector<NodeGraph::ComputeEvent> NodeGraph::drain_compute_events() {
    std::lock_guard<std::mutex> lk(event_mutex_);
    std::vector<ComputeEvent> out;
    out.swap(event_buffer_);
    return out;
}

} // namespace ps
