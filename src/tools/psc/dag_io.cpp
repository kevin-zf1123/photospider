#include "psc/dag_io.hpp"

#include <charconv>
#include <cstring>
#include <string>
#include <utility>

#include "common/codec.hpp"
#include "common/process.hpp"

namespace ps::tools::psc {
namespace {
ParameterValue parameter(Json value) {
  if (value.is_string())
    return value.text();
  if (value.is_bool())
    return value.boolean();
  if (value.is_int())
    return value.i64();
  if (!value.is_object())
    return value.real();
  value.fields({"type", "value"});
  auto type = value.at("type").text();
  auto data = value.at("value");
  if (type == "string")
    return data.text();
  if (type == "bool")
    return data.boolean();
  if (type == "i64") {
    if (!data.is_string())
      return data.i64();
    auto text = data.text();
    std::int64_t n = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size())
      throw ToolError("invalid i64 parameter");
    return n;
  }
  if (type == "f64")
    return data.real();
  if (type == "f64_bits") {
    auto text = data.text();
    std::uint64_t bits = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), bits, 16);
    if (text.size() != 16 || r.ec != std::errc{} ||
        r.ptr != text.data() + text.size())
      throw ToolError("f64_bits requires sixteen hexadecimal digits");
    double number;
    std::memcpy(&number, &bits, sizeof(number));
    return number;
  }
  throw ToolError("unknown parameter type: " + type);
}
}  // namespace
namespace {
std::string header_token(const std::string& text, std::size_t* position) {
  for (;;) {
    *position = text.find_first_not_of(" \t\r\n", *position);
    if (*position == std::string::npos)
      return {};
    if (text[*position] != ';')
      break;
    *position = text.find('\n', *position);
    if (*position == std::string::npos)
      return {};
  }
  auto end = text.find_first_of(" \t\r\n;", *position);
  auto result =
      text.substr(*position, end == std::string::npos ? end : end - *position);
  *position = end;
  return result;
}
}  // namespace
std::string detect(const std::string& text) {
  auto start = text.find_first_not_of(" \t\r\n");
  if (start != std::string::npos && text[start] == '{')
    return "dag";
  std::size_t position = 0;
  if (header_token(text, &position) == "psir")
    return "ir";
  throw ToolError(
      "unrecognized input; expected DAG JSON or psir v4 (legacy YAML is "
      "unsupported)");
}
std::string inspect_input(const std::string& text) {
  auto type = detect(text);
  if (type == "dag") {
    // Check only the encoding and format tag, never workflow schemas or nodes.
    JsonDocument document(text);
    format(document.root(), "photospider.dag");
  } else {
    std::size_t position = 0;
    header_token(text, &position);
    if (header_token(text, &position) != "4")
      throw ToolError("unsupported psir version; re-export using artifact v4");
  }
  return type;
}
WorkflowDocument read_dag(const std::string& text) {
  JsonDocument document(text);
  auto root = document.root();
  root.fields(
      {"format", "version", "schema_version", "inputs", "nodes", "outputs"});
  format(root, "photospider.dag");
  WorkflowDocument result;
  result.schema_version = u32(root.at("schema_version"));
  auto nodes = root.at("nodes").array();
  auto inputs = root.at("inputs").array();
  auto outputs = root.at("outputs").array();
  if (nodes.size() > 65536 || inputs.size() > 4096 || outputs.size() > 4096)
    throw ToolError("DAG count limits exceeded");
  for (auto input : inputs) {
    input.fields({"id", "name", "schema"});
    result.inputs.push_back({input.at("id").u64(), input.at("name").text(),
                             schema(input.at("schema"))});
  }
  for (auto node : nodes) {
    interrupted();
    WorkflowNode item;
    item.id = node.at("id").u64();
    try {
      node.fields({"id", "operation", "inputs", "parameters"});
      item.operation = node.at("operation").text();
      for (auto input : node.at("inputs").array()) {
        if (input.optional("input")) {
          input.fields({"input"});
          item.inputs.push_back(
              WorkflowInputReference{input.at("input").u64()});
        } else {
          input.fields({"node", "port"});
          item.inputs.push_back(WorkflowNodeOutput{input.at("node").u64(),
                                                   input.at("port").text()});
        }
      }
      for (const auto& p : node.at("parameters").object())
        item.parameters.emplace(p.first, parameter(p.second));
    } catch (ToolError& error) {
      error.node = item.id;
      throw;
    }
    result.nodes.push_back(std::move(item));
  }
  for (auto output : outputs) {
    output.fields({"name", "node", "port"});
    result.outputs.push_back({output.at("name").text(), output.at("node").u64(),
                              output.at("port").text()});
  }
  return result;
}
}  // namespace ps::tools::psc
