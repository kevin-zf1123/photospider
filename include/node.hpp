#pragma once
#include "ps_types.hpp"

namespace ps {

class Node {
public:
    int id = -1;
    std::string name;
    std::string type;
    std::string subtype;

    // --- MODIFIED: Inputs are now split into two categories for clarity and function ---
    std::vector<ImageInput> image_inputs;
    std::vector<ParameterInput> parameter_inputs;
    
    // --- DEPRECATED: Old unified input model ---
    // std::vector<InputPort> inputs; 

    // Static parameters defined in the YAML file.
    YAML::Node parameters;
    
    // --- NEW: Parameters populated at runtime from upstream nodes ---
    // This is a combination of static `parameters` and dynamic `parameter_inputs`.
    YAML::Node runtime_parameters;

    std::vector<OutputPort> outputs;
    std::vector<CacheEntry> caches;

    // --- MODIFIED: The in-memory cache now holds the entire NodeOutput ---
    std::optional<NodeOutput> cached_output;

    // --- DEPRECATED: Old image-only cache ---
    // cv::Mat image_matrix;

    static Node from_yaml(const YAML::Node& n);
    YAML::Node to_yaml() const;
};

} // namespace ps