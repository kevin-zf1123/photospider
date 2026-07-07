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
  GraphModel::NodeMap loaded_nodes;
  for (const auto& node_yaml : config) {
    Node node = Node::from_yaml(node_yaml);
    if (loaded_nodes.count(node.id)) {
      throw GraphError(
          GraphErrc::InvalidYaml,
          "Duplicate node id " + std::to_string(node.id) + " in graph YAML.");
    }
    loaded_nodes[node.id] = std::move(node);
  }
  graph.replace_nodes(std::move(loaded_nodes));
}

void GraphIOService::save(const GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  YAML::Node root(YAML::NodeType::Sequence);
  for (int id : graph.node_ids()) {
    root.push_back(graph.node(id).to_yaml());
  }

  std::ofstream fout(yaml_path);
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to open file for writing: " + yaml_path.string());
  }
  fout << root;
}

}  // namespace ps
