#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/file_io.hpp"
#include "common/json.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/plugin/operation_program.hpp"
#include "psx/bindings_io.hpp"
#include "psx/options.hpp"
#include "psx/result_export.hpp"

namespace {
void tiled_bindings(const ps::tools::fs::path& directory) {
  using namespace ps;         // NOLINT(build/namespaces)
  using namespace ps::tools;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  SchemaTemplate schema;
  schema.id = "test.binding";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {3, 5}};
  schema.tensors.push_back(tensor);
  OperationPortConstraint port;
  port.result_schema_id = schema.id;
  port.result_schema_version = 1;
  OperationDefinition definition;
  definition.key = "copy";
  definition.traits.input_count = 1;
  definition.traits.input_schema = {port};
  definition.traits.outputs[0].output_schema = port;
  definition.traits.outputs[0].result_schema = schema;
  definition.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto index : member.second) {
        auto task = take(builder.add_copy());
        check(builder.read_input(task, 0, member.first, index));
        check(builder.write_output(task, 0, member.first, index));
      }
    return builder.finish();
  };
  check(registry->register_operation(std::move(definition)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      {1, "input", std::make_shared<const SchemaTemplate>(schema)}};
  document.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  PlanningOptions options;
  options.tile_extent = 2;
  options.maximum_tasks = 2;
  options.output_requests["result"].members[0] =
      take(TileSet::range(ResourceBudget{}, 1, 3));
  auto plan =
      take(compiler.plan(take(compiler.analyze(graph.snapshot())), options));
  options.maximum_tasks = 1;
  if (compiler.plan(take(compiler.analyze(graph.snapshot())), options)
          .status()
          .code != ErrorCode::ResourceExhausted)
    throw std::runtime_error("task limit did not bound physical tasks");
  ExecutionContext context(registry);
  Files files;
  std::vector<std::uint64_t> bits(15);
  std::string bytes;
  for (std::size_t i = 0; i < bits.size(); ++i) {
    bits[i] =
        i % 2 ? UINT64_C(0x8000000000000000) : UINT64_C(0x7ff8000000000000) + i;
    for (unsigned byte = 0; byte < 8; ++byte)
      bytes.push_back(static_cast<char>(bits[i] >> (byte * 8)));
  }
  files.write(directory / "input.bin", bytes);
  files.write(directory / "bindings.json",
              R"({"format":"photospider.bindings","version":1,"inputs":[)"
              R"({"name":"input","tensors":[{"slot":0,"path":"input.bin",)"
              R"("encoding":"raw-le","dtype":"f64","shape":[3,5]}]}]})");
  auto planners = std::make_shared<PlannerRegistry>();
  auto padded = *make_default_planner_registry()->find("static-tiles");
  auto base = padded.plan;
  padded.name = "padded-input";
  padded.plan = [base](const auto& ir, auto& builder, const auto& options) {
    auto status = base(ir, builder, options);
    if (!status.ok())
      return status;
    auto& p = builder.program();
    p.tiles[p.values[p.inputs[0]].tiles[0]].bytes += 8;
    return physical::finalize_program(&p);
  };
  check(planners->add(std::move(padded)));
  planners->freeze();
  Compiler padded_compiler(registry, {}, planners);
  options.maximum_tasks = 2;
  options.planner = "padded-input";
  auto padded_plan = take(padded_compiler.plan(
      take(padded_compiler.analyze(graph.snapshot())), options));
  bool rejected_layout = false;
  try {
    ps::tools::psx::read_bindings((directory / "bindings.json").string(),
                                  padded_plan, &files,
                                  take(context.resource_budget()));
  } catch (const ToolError& error) {
    rejected_layout = error.id == "PS-TypeMismatch";
  }
  if (!rejected_layout)
    throw std::runtime_error(
        "binding accepted incompatible compiled allocation");
  auto bindings = ps::tools::psx::read_bindings(
      (directory / "bindings.json").string(), plan, &files,
      take(context.resource_budget()));
  std::set<const CpuStorage*> storage;
  std::uint64_t retained = 0;
  auto input = bindings.inputs[0].result;
  auto facts = take(input.descriptor());
  for (TileIndex index : {1U, 2U}) {
    auto region = take(plan.input_tile_grids()[0][0].region(index));
    auto view =
        take(take(input.acquire_tensor(facts, 0, region)).affine_view());
    if (!storage.insert(view.storage().get()).second)
      throw std::runtime_error("binding tiles share an allocation");
    retained += view.storage()->bytes().size();
  }
  if (retained != 6 * sizeof(double))
    throw std::runtime_error("binding copied unrequested tiles");
  auto run =
      take(context.submit(take(context.prepare(plan)), std::move(bindings)));
  auto value = take(run.join()).results.at("result");
  auto descriptor = take(value.descriptor());
  for (std::uint64_t y = 0; y < 2; ++y)
    for (std::uint64_t x = 2; x < 5; ++x) {
      std::uint64_t actual = 0;
      check(value.read_tensor(descriptor, 0, {y, x}, &actual, sizeof(actual)));
      if (actual != bits[y * 5 + x])
        throw std::runtime_error("binding changed raw bits or coordinates");
    }
  options.planner = "static-tiles";
  options.maximum_tasks = 4;
  options.input_layouts["input"] = physical::InputLayout::DenseHost;
  auto dense =
      take(compiler.plan(take(compiler.analyze(graph.snapshot())), options));
  auto dense_bindings = ps::tools::psx::read_bindings(
      (directory / "bindings.json").string(), dense, &files,
      take(context.resource_budget()));
  auto dense_input = dense_bindings.inputs[0].result;
  auto dense_facts = take(dense_input.descriptor());
  const CpuStorage* shared = nullptr;
  for (TileIndex index : {1U, 2U}) {
    auto region = take(dense.input_tile_grids()[0][0].region(index));
    auto view = take(
        take(dense_input.acquire_tensor(dense_facts, 0, region)).affine_view());
    if ((shared && shared != view.storage().get()) ||
        view.storage()->bytes().size() != 120)
      throw std::runtime_error(
          "dense binding did not retain full member backing");
    shared = view.storage().get();
  }
  auto dense_run = take(
      context.submit(take(context.prepare(dense)), std::move(dense_bindings)));
  auto dense_result = take(dense_run.join());
  if (dense_result.diagnostics.completed_tasks != 4)
    throw std::runtime_error(
        "dense binding did not execute explicit import COPYs");
  auto dense_value = dense_result.results.at("result");
  auto dense_descriptor = take(dense_value.descriptor());
  for (std::uint64_t y = 0; y < 2; ++y)
    for (std::uint64_t x = 2; x < 5; ++x) {
      std::uint64_t actual = 0;
      check(dense_value.read_tensor(dense_descriptor, 0, {y, x}, &actual,
                                    sizeof(actual)));
      if (actual != bits[y * 5 + x])
        throw std::runtime_error("import COPY changed raw bits or coordinates");
    }
}
void worker_options() {
  using namespace ps::tools;  // NOLINT(build/namespaces)
  for (const auto* option : {"--threads", "--parallelism"}) {
    for (const auto* value : {"0", "4097", "4294967295"}) {
      std::vector<std::string> args = {"psx", "--help", option, value};
      std::vector<char*> argv;
      for (auto& arg : args)
        argv.push_back(arg.data());
      bool json = false, rejected = false;
      try {
        psx::parse_options(static_cast<int>(argv.size()), argv.data(), &json);
      } catch (const ToolError& error) {
        rejected = error.exit_code == 2;
      }
      if (!rejected)
        throw std::runtime_error("worker count escaped argument validation");
    }
  }
}
}  // namespace

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
  worker_options();
  tiled_bindings(directory);
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
