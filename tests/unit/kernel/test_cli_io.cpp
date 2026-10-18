#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/file_io.hpp"
#include "common/json.hpp"
#include "psx/result_export.hpp"

int main() try {
  using namespace ps;         // NOLINT(build/namespaces)
  using namespace ps::tools;  // NOLINT(build/namespaces)
  auto directory =
      fs::temp_directory_path() /
      ("ps-cli-io-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(directory);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } cleanup{directory};
  ResourceBudget budget;
  SchemaTemplate schema;
  schema.id = "test.bits";
  schema.version = 1;
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {262144}};
  schema.tensors.push_back(tensor);
  std::vector<std::uint64_t> bits(262144);
  for (std::size_t i = 0; i < bits.size(); ++i)
    bits[i] =
        i % 2 ? UINT64_C(0x8000000000000000) : UINT64_C(0x7ff8000000000042);
  auto builder = take(ResultBuilder::start(budget, schema, "io"));
  check(builder.publish_tensor(
      0, Region::whole({bits.size()}),
      {reinterpret_cast<const std::uint8_t*>(bits.data()), bits.size() * 8}));
  ExecutionResult result;
  result.results.emplace("result", take(builder.seal()));
  Files files;
  auto output = directory / "result";
  files.reserve(output);
  ps::tools::psx::export_results(result, output, files, {}, budget);
  JsonDocument manifest(files.read(output / "manifest.json"));
  auto fragments = manifest.root()
                       .at("outputs")
                       .array()[0]
                       .at("tensors")
                       .array()[0]
                       .at("fragments")
                       .array();
  if (fragments.size() < 2)
    throw std::runtime_error("export did not split a large window");
  std::vector<bool> seen(bits.size());
  for (auto fragment : fragments) {
    auto bytes = files.read(output / fragment.at("path").text());
    if (bytes.size() > 1024 * 1024)
      throw std::runtime_error("unbounded export buffer");
    auto offset = fragment.at("offset").array()[0].u64();
    auto count = fragment.at("shape").array()[0].u64();
    if (bytes.size() != count * 8 || offset + count > bits.size())
      throw std::runtime_error("fragment descriptor mismatch");
    for (std::size_t i = 0; i < count; ++i) {
      std::uint64_t value = 0;
      for (unsigned byte = 0; byte < 8; ++byte)
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(bytes[i * 8 + byte]))
                 << (8 * byte);
      if (seen[offset + i] || value != bits[offset + i])
        throw std::runtime_error("overlap or changed IEEE bits");
      seen[offset + i] = true;
    }
  }
  for (bool value : seen)
    if (!value)
      throw std::runtime_error("missing coverage");
  ExecutionResult invalid_name;
  invalid_name.results.emplace(std::string("output-") + static_cast<char>(0xff),
                               result.results.at("result"));
  const auto invalid_output = directory / "invalid-name";
  files.reserve(invalid_output);
  bool rejected_name = false;
  try {
    ps::tools::psx::export_results(invalid_name, invalid_output, files, {},
                                   budget);
  } catch (const ToolError& error) {
    rejected_name = error.id == "PS-JSON-UTF8";
  }
  if (!rejected_name || fs::exists(invalid_output))
    throw std::runtime_error("result export silently changed its output name");
  for (const auto& entry : fs::directory_iterator(directory))
    if (entry.path().filename().string().rfind(".psx-", 0) == 0)
      throw std::runtime_error("failed semantic JSON export left staging");
  fs::create_directory(directory / "staged");
  fs::create_directory(directory / "occupied");
  bool refused = false;
  try {
    publish(directory / "staged", directory / "occupied", true);
  } catch (const fs::filesystem_error&) {
    refused = true;
  }
  if (!refused || !fs::exists(directory / "staged"))
    throw std::runtime_error("exclusive publication replaced a directory");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
