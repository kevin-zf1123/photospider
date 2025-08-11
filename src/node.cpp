#include "node.hpp"

namespace ps {

// --- NEW: Helper to serialize new input/output structures to YAML ---

static YAML::Node image_inputs_to_yaml(const std::vector<ImageInput>& inputs) {
    YAML::Node arr(YAML::NodeType::Sequence);
    for (const auto& p : inputs) {
        YAML::Node n;
        n["from_node_id"] = p.from_node_id;
        if (p.from_output_name != "image") { // Only write if not default
            n["from_output_name"] = p.from_output_name;
        }
        arr.push_back(n);
    }
    return arr;
}

static YAML::Node parameter_inputs_to_yaml(const std::vector<ParameterInput>& inputs) {
    YAML::Node arr(YAML::NodeType::Sequence);
    for (const auto& p : inputs) {
        YAML::Node n;
        n["from_node_id"] = p.from_node_id;
        n["from_output_name"] = p.from_output_name;
        n["to_parameter_name"] = p.to_parameter_name;
        arr.push_back(n);
    }
    return arr;
}

static YAML::Node ports_to_yaml(const std::vector<OutputPort>& outs) {
    YAML::Node arr(YAML::NodeType::Sequence);
    for (const auto& p : outs) {
        YAML::Node n;
        n["output_id"] = p.output_id;
        n["output_type"] = p.output_type;
        if (p.output_parameters) n["output_parameters"] = p.output_parameters;
        arr.push_back(n);
    }
    return arr;
}

static YAML::Node caches_to_yaml(const std::vector<CacheEntry>& caches) {
    YAML::Node arr(YAML::NodeType::Sequence);
    for (const auto& c : caches) {
        YAML::Node n;
        n["cache_type"] = c.cache_type;
        n["location"] = c.location;
        arr.push_back(n);
    }
    return arr;
}

// --- MODIFIED: from_yaml now parses the new dynamic input format ---
Node Node::from_yaml(const YAML::Node& n) {
    Node node;
    node.id = n["id"].as<int>();
    node.name = n["name"].as<std::string>("");
    node.type = n["type"].as<std::string>("");
    node.subtype = n["subtype"].as<std::string>("");

    if (n["image_inputs"]) {
        for (const auto& it : n["image_inputs"]) {
            ImageInput p;
            p.from_node_id = it["from_node_id"].as<int>(-1);
            p.from_output_name = it["from_output_name"].as<std::string>("image");
            node.image_inputs.push_back(std::move(p));
        }
    }

    if (n["parameter_inputs"]) {
        for (const auto& it : n["parameter_inputs"]) {
            ParameterInput p;
            p.from_node_id = it["from_node_id"].as<int>(-1);
            p.from_output_name = it["from_output_name"].as<std::string>();
            p.to_parameter_name = it["to_parameter_name"].as<std::string>();
            if (p.from_output_name.empty() || p.to_parameter_name.empty()) {
                throw GraphError("Parameter input for node " + std::to_string(node.id) + " is missing required fields.");
            }
            node.parameter_inputs.push_back(std::move(p));
        }
    }

    if (n["parameters"]) node.parameters = n["parameters"];

    if (n["outputs"]) {
        for (const auto& ot : n["outputs"]) {
            OutputPort p;
            p.output_id = ot["output_id"].as<int>(-1);
            p.output_type = ot["output_type"].as<std::string>("");
            if (ot["output_parameters"]) p.output_parameters = ot["output_parameters"];
            node.outputs.push_back(std::move(p));
        }
    }

    if (n["caches"]) {
        for (const auto& ct : n["caches"]) {
            CacheEntry c;
            c.cache_type = ct["cache_type"].as<std::string>("");
            c.location = ct["location"].as<std::string>("");
            node.caches.push_back(std::move(c));
        }
    }

    return node;
}

// --- MODIFIED: to_yaml now writes the new dynamic input format ---
YAML::Node Node::to_yaml() const {
    YAML::Node n;
    n["id"] = id;
    n["name"] = name;
    n["type"] = type;
    n["subtype"] = subtype;

    if (!image_inputs.empty()) n["image_inputs"] = image_inputs_to_yaml(image_inputs);
    if (!parameter_inputs.empty()) n["parameter_inputs"] = parameter_inputs_to_yaml(parameter_inputs);
    
    if (parameters && parameters.IsMap() && parameters.size() > 0) {
         n["parameters"] = parameters;
    } else {
        // Ensure parameters is always a map, even if empty, for consistency.
        n["parameters"] = YAML::Node(YAML::NodeType::Map);
    }
    
    if (!outputs.empty()) n["outputs"] = ports_to_yaml(outputs);
    if (!caches.empty()) n["caches"] = caches_to_yaml(caches);
    
    return n;
}

} // namespace ps