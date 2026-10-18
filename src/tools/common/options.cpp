#include "common/options.hpp"

#include <charconv>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ps::tools {
namespace {
struct OptionToken {
  std::string name, value;
  bool joined = false;
};
OptionToken split_option(std::string argument,
                         const std::set<std::string>& values) {
  OptionToken token{std::move(argument), {}};
  if (token.name.size() > 2 && token.name[1] != '-' &&
      values.count(token.name.substr(0, 2))) {
    const auto begin = token.name[2] == '=' ? 3U : 2U;
    token.value = token.name.substr(begin);
    token.name.resize(2);
    token.joined = true;
  } else {
    const auto equals = token.name.find('=');
    if (equals != std::string::npos) {
      token.value = token.name.substr(equals + 1);
      token.name.resize(equals);
      token.joined = true;
    }
  }
  return token;
}
bool json_diagnostics_requested(int argc, char** argv,
                                const std::set<std::string>& values) {
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--")
      break;
    if (argument.empty() || argument.front() != '-')
      continue;
    auto token = split_option(std::move(argument), values);
    if (!values.count(token.name))
      continue;
    if (!token.joined) {
      if (++i == argc)
        break;
      token.value = argv[i];
    }
    // The first occurrence determines rendering of duplicate-option errors.
    if (token.name == "--diagnostic-format")
      return token.value == "json";
  }
  return false;
}
}  // namespace
std::uint64_t unsigned_number(const std::string& text,
                              const std::string& label) {
  std::uint64_t result = 0;
  auto end = text.data() + text.size();
  auto parsed = std::from_chars(text.data(), end, result);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end)
    throw ToolError("invalid unsigned integer for " + label + ": " + text, 2,
                    "PS-DRIVER-OPTION");
  return result;
}
ParsedOptions parse_arguments(int argc, char** argv, bool* json_diagnostics,
                              std::set<std::string> flags,
                              std::set<std::string> values,
                              const std::set<std::string>& repeatable,
                              bool allow_warnings) {
  flags.insert(
      {"--check", "--time-report", "--version", "--help", "--list-operations"});
  values.insert({"-o", "--outputs", "--requests", "--load-ops", "--resources",
                 "--diagnostic-format", "--color"});
  values.insert(repeatable.begin(), repeatable.end());
  if (json_diagnostics)
    *json_diagnostics = json_diagnostics_requested(argc, argv, values);
  ParsedOptions parsed;
  auto& o = parsed.common;
  bool positional = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (!positional && arg == "--") {
      positional = true;
      continue;
    }
    if (!positional && arg == "-h")
      arg = "--help";
    if (!positional && arg == "-v" && flags.count("--verbose"))
      arg = "--verbose";
    if (!positional && allow_warnings && arg.rfind("-W", 0) == 0) {
      o.warnings.push_back(arg);
      continue;
    }
    if (positional || arg == "-" || arg.empty() || arg.front() != '-') {
      if (!o.input.empty())
        throw ToolError("exactly one input is supported", 2);
      o.input = arg;
      continue;
    }
    auto token = split_option(std::move(arg), values);
    arg = std::move(token.name);
    auto value = std::move(token.value);
    const auto joined = token.joined;
    if (flags.count(arg)) {
      if (joined || !o.flags.insert(arg).second)
        throw ToolError("duplicate or valued flag: " + arg, 2);
      continue;
    }
    if (!values.count(arg))
      throw ToolError("unknown option: " + arg, 2);
    if (!joined) {
      if (++i == argc)
        throw ToolError("missing argument for " + arg, 2);
      value = argv[i];
    }
    if (value.empty())
      throw ToolError("empty argument for " + arg, 2);
    if (arg == "--load-ops") {
      o.libraries.push_back(value);
      continue;
    }
    if (repeatable.count(arg)) {
      parsed.repeated.emplace_back(arg, value);
      continue;
    }
    if (!o.values.emplace(arg, value).second)
      throw ToolError("duplicate option: " + arg, 2);
  }
  o.output = o.get("-o");
  if (o.get("--diagnostic-format", "text") != "text" &&
      o.get("--diagnostic-format") != "json")
    throw ToolError("invalid diagnostic format", 2);
  if (o.get("--color", "auto") != "auto" && o.get("--color") != "always" &&
      o.get("--color") != "never")
    throw ToolError("invalid color mode", 2);
  if (o.has("--outputs") && o.has("--requests"))
    throw ToolError("--outputs conflicts with --requests", 2);

  return parsed;
}
std::vector<std::string> output_names(const std::string& text) {
  std::vector<std::string> result;
  std::set<std::string> seen;
  std::size_t start = 0;
  do {
    auto end = text.find(',', start);
    auto name =
        text.substr(start, end == std::string::npos ? end : end - start);
    if (name.empty() || !seen.insert(name).second)
      throw ToolError("empty or repeated output name", 2);
    result.push_back(name);
    if (end == std::string::npos)
      break;
    start = end + 1;
  } while (true);
  return result;
}
std::uint32_t positive_count(const CommonOptions& o, const std::string& name,
                             std::uint32_t fallback) {
  if (!o.has(name))
    return fallback;
  auto n = unsigned_number(o.get(name), name);
  if (!n || n > UINT32_MAX)
    throw ToolError(name + " requires 1..UINT32_MAX", 2);
  return static_cast<std::uint32_t>(n);
}
void require_input(const CommonOptions& options, bool information) {
  if (!information && options.input.empty())
    throw ToolError("an input file is required", 2);
}
}  // namespace ps::tools
