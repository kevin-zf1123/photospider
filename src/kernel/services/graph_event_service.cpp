#include "kernel/services/graph_event_service.hpp"

namespace ps {

void GraphEventService::push(int id,
                             const std::string& name,
                             const std::string& source,
                             double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push_back(ComputeEvent{ id, name, source, ms });
}

std::vector<GraphEventService::ComputeEvent> GraphEventService::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ComputeEvent> out;
    out.swap(buffer_);
    return out;
}

} // namespace ps

