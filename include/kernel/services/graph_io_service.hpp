#pragma once

#include <filesystem>

#include "graph_model.hpp"

namespace ps {

class GraphIOService {
 public:
  void load(GraphModel& graph, const std::filesystem::path& yaml_path) const;
  void save(const GraphModel& graph,
            const std::filesystem::path& yaml_path) const;
};

}  // namespace ps
