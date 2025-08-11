#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <optional>
#include <sstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>

namespace ps {
namespace fs = std::filesystem;

// The standard output for any data-producing entity (e.g., a file or a parameter).
// YAML::Node is used for its flexibility in holding strings, numbers, bools, etc.
using OutputValue = YAML::Node;

// Defines a connection for an image input.
struct ImageInput {
    int from_node_id = -1;
    // The named output from the source node to connect to. Defaults to "image".
    std::string from_output_name = "image";
};

// Defines a connection for a parameter input, linking a source node's output
// to one of this node's runtime parameters.
struct ParameterInput {
    int from_node_id = -1;
    // The named output from the source node (e.g., "width", "result").
    std::string from_output_name;
    // The name of the parameter to set on the target node (e.g., "ksize").
    std::string to_parameter_name;
};


// Deprecated: Old input port structure. Replaced by ImageInput and ParameterInput.
struct InputPort {
    int input_id = -1;
    std::string input_type;
    YAML::Node input_parameters;
};

struct OutputPort {
    int output_id = -1;
    std::string output_type;
    YAML::Node output_parameters;
};

struct CacheEntry {
    std::string cache_type;
    std::string location;
};

// The complete result of a node's computation. It can include a primary image
// as well as any number of named data outputs.
struct NodeOutput {
    cv::Mat image_matrix; // The primary image output
    std::unordered_map<std::string, OutputValue> data; // Other named data outputs
};

struct GraphError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Node;
class NodeGraph;

// --- MODIFIED: The OpFunc signature now returns the comprehensive NodeOutput struct ---
// It still receives the full node to access runtime_parameters and a vector of input images.
using OpFunc = std::function<NodeOutput(const Node&, const std::vector<cv::Mat>&)>;

class OpRegistry {
public:
    static OpRegistry& instance();
    void register_op(const std::string& type, const std::string& subtype, OpFunc fn);
    std::optional<OpFunc> find(const std::string& type, const std::string& subtype) const;
    std::vector<std::string> get_keys() const;
private:
    std::unordered_map<std::string, OpFunc> table_;
};

inline std::string make_key(const std::string& type, const std::string& subtype) {
    return type + ":" + subtype;
}

} // namespace ps