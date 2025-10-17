#include "kernel/services/graph_io_service.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>

#include "graph_model.hpp"

namespace ps {

void GraphIOService::load(GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  YAML::Node config;
  try {
    config = YAML::LoadFile(yaml_path.string());
  } catch (const std::exception& e) {
    throw GraphError(GraphErrc::Io, "Failed to load YAML file " +
                                        yaml_path.string() + ": " + e.what());
  }
  if (!config.IsSequence()) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "YAML root is not a sequence of nodes.");
  }
  graph.clear();
  for (const auto& node_yaml : config) {
    Node node = Node::from_yaml(node_yaml);
    graph.add_node(node);
  }
}

void GraphIOService::save(const GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  YAML::Node root(YAML::NodeType::Sequence);
  std::vector<int> sorted_ids;
  sorted_ids.reserve(graph.nodes.size());
  for (const auto& pair : graph.nodes) {
    sorted_ids.push_back(pair.first);
  }
  std::sort(sorted_ids.begin(), sorted_ids.end());
  for (int id : sorted_ids) {
    root.push_back(graph.nodes.at(id).to_yaml());
  }

  std::ofstream fout(yaml_path);
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to open file for writing: " + yaml_path.string());
  }
  fout << root;
}

}  // namespace ps
