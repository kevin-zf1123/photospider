#pragma once

#include <filesystem>
#include <string>

#include "graph_model.hpp"

namespace ps {

class GraphCacheService {
public:
    std::filesystem::path node_cache_dir(const GraphModel& graph, int node_id) const;

    GraphModel::DriveClearResult clear_drive_cache(GraphModel& graph) const;
    GraphModel::MemoryClearResult clear_memory_cache(GraphModel& graph) const;
    void clear_cache(GraphModel& graph) const;
    GraphModel::CacheSaveResult cache_all_nodes(GraphModel& graph, const std::string& cache_precision) const;
    GraphModel::MemoryClearResult free_transient_memory(GraphModel& graph) const;
    GraphModel::DiskSyncResult synchronize_disk_cache(GraphModel& graph, const std::string& cache_precision) const;

    void save_cache_if_configured(GraphModel& graph, const Node& node, const std::string& cache_precision) const;
    bool try_load_from_disk_cache(GraphModel& graph, Node& node) const;
    bool try_load_from_disk_cache_into(GraphModel& graph, const Node& node, NodeOutput& out) const;
};

} // namespace ps
