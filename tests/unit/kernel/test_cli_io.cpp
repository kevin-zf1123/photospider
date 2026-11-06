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
std::shared_ptr<ps::OperationRegistry> copy_registry(
    const ps::SchemaTemplate& schema) {
  using namespace ps;         // NOLINT(build/namespaces)
  using namespace ps::tools;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
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
  return registry;
}
ps::WorkflowDocument copy_document(const ps::SchemaTemplate& schema) {
  using namespace ps;  // NOLINT(build/namespaces)
  WorkflowDocument document;
  document.inputs = {
      {1, "input", std::make_shared<const SchemaTemplate>(schema)}};
  document.nodes = {{1, "copy", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  return document;
}
ps::ExecutionPlan export_plan(const ps::SchemaTemplate& schema,
                              std::uint64_t extent) {
  ps::GraphContext graph(copy_document(schema));
  ps::Compiler compiler(copy_registry(schema));
  ps::PlanningOptions options;
  options.tile_extent = extent;
  return ps::tools::take(compiler.plan(
      ps::tools::take(compiler.analyze(graph.snapshot())), options));
}
void tiled_bindings(const ps::tools::fs::path& directory) {
  using namespace ps;         // NOLINT(build/namespaces)
  using namespace ps::tools;  // NOLINT(build/namespaces)
  SchemaTemplate schema;
  schema.id = "test.binding";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {3, 5}};
  schema.tensors.push_back(tensor);
  auto registry = copy_registry(schema);
  auto document = copy_document(schema);
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
std::uint64_t header_u64(const std::string& file, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(file.at(offset + i)))
             << (8 * i);
  return value;
}
void check_header(const std::string& file, std::uint64_t extent,
                  std::uint64_t count) {
  if (file.compare(0, 8, "PSXTILE2", 8) || header_u64(file, 8) != extent ||
      header_u64(file, 16) != count || file.size() < 24 + 24 * count)
    throw std::runtime_error("invalid output header");
}
void check_record(const std::string& file, unsigned record, unsigned slot,
                  std::uint64_t id, std::uint64_t length) {
  const auto pos = 24 + record * 24;
  if (header_u64(file, pos) != slot || header_u64(file, pos + 8) != id ||
      header_u64(file, pos + 16) != length)
    throw std::runtime_error("invalid output tile record");
}
void tiled_export(const ps::tools::fs::path& directory) {
  using namespace ps;         // NOLINT(build/namespaces)
  using namespace ps::tools;  // NOLINT(build/namespaces)
  ResourceBudget budget;
  Files files;
  SchemaTemplate schema;
  schema.id = "test.export";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {2, 4}};
  schema.tensors.push_back(member);
  auto metadata = member;
  metadata.key = "tag";
  metadata.descriptor.shape = {1};
  schema.tensors.push_back(metadata);
  const auto plan = export_plan(schema, 2);
  const auto& grid = plan.output("result").tile_grids[0];
  for (bool reversed : {false, true}) {
    auto builder = take(ResultBuilder::start(budget, schema, "export", {}, 2));
    std::vector<std::string> payloads{"abAB", "cdCD"};
    for (unsigned id = 0; id < 2; ++id) {
      auto box = take(grid.region(id));
      auto buffer = take(budget.allocator().allocate(4));
      std::memcpy(buffer.data(), payloads[id].data(), 4);
      StridedLayout layout = reversed ? StridedLayout{3, {-2, -1}, {0, 2 * id}}
                                      : StridedLayout{0, {2, 1}, {0, 2 * id}};
      check(builder.publish_tensor(0, box, layout, std::move(buffer).freeze()));
    }
    const std::uint8_t tag = 123;
    check(builder.publish_tensor(1, Region::whole({1}), ByteView(&tag, 1)));
    ExecutionResult result;
    result.results.emplace("result", take(builder.seal()));
    auto output = directory / (reversed ? "reversed" : "tiled");
    psx::export_results(plan, result, output, files, {}, budget);
    JsonDocument manifest(files.read(output / "manifest.json"));
    auto entry = manifest.root().at("outputs").array()[0];
    auto file = files.read(output / entry.at("path").text());
    check_header(file, 2, 3);
    check_record(file, 2, 1, 0, 1);
    if (file.size() != 96 + 9 || entry.at("byte_length").u64() != file.size() ||
        file.back() != 123)
      throw std::runtime_error("second tensor missing from shared output bin");
    auto tiles = entry.at("tensors").array()[0].at("tiles").array();
    if (tiles.size() != 2)
      throw std::runtime_error("expected two complete tiles");
    for (unsigned id = 0; id < 2; ++id) {
      auto tile = tiles[id];
      check_record(file, id, 0, id, 4);
      if (tile.at("index").u64() != id ||
          tile.at("offset").array()[1].u64() != 2 * id ||
          tile.at("byte_strides").array()[0].i64() != (reversed ? -2 : 2) ||
          tile.at("file_offset").u64() != 96 + 4 * id ||
          file.substr(tile.at("file_offset").u64(), 4) != payloads[id])
        throw std::runtime_error(
            "tile export changed physical bytes or layout");
    }
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
  tiled_export(directory);
  ResourceBudget budget;
  SchemaTemplate schema;
  schema.id = "test.bits";
  schema.version = 1;
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {262145}};
  schema.tensors.push_back(tensor);
  std::vector<std::uint64_t> bits(262145);
  for (std::size_t i = 0; i < bits.size(); ++i)
    bits[i] =
        i % 2 ? UINT64_C(0x8000000000000000) : UINT64_C(0x7ff8000000000042);
  auto builder = take(ResultBuilder::start(budget, schema, "io", {}, 262144));
  check(builder.publish_tensor(
      0, Region::whole({bits.size()}),
      {reinterpret_cast<const std::uint8_t*>(bits.data()), bits.size() * 8}));
  ExecutionResult result;
  result.results.emplace("result", take(builder.seal()));
  Files files;
  auto output = directory / "result";
  files.reserve(output);
  const auto plan = export_plan(schema, 262144);
  ps::tools::psx::export_results(plan, result, output, files, {}, budget);
  JsonDocument manifest(files.read(output / "manifest.json"));
  auto entry = manifest.root().at("outputs").array()[0];
  auto file = files.read(output / entry.at("path").text());
  check_header(file, 262144, 2);
  auto tiles = entry.at("tensors").array()[0].at("tiles").array();
  if (entry.at("byte_length").u64() != 72 + bits.size() * 8 ||
      file.size() != entry.at("byte_length").u64())
    throw std::runtime_error("large output bin size mismatch");
  if (tiles.size() != 2)
    throw std::runtime_error("expected two complete tiles in the output bin");
  for (unsigned id = 0; id < 2; ++id) {
    auto tile = tiles[id];
    auto offset = id * 262144U;
    auto count = id == 0 ? 262144U : 1U;
    check_record(file, id, 0, id, count * 8);
    if (tile.at("shape").array()[0].u64() != count ||
        tile.at("offset").array()[0].u64() != offset ||
        tile.at("file_offset").u64() != 72 + offset * 8 ||
        std::memcmp(file.data() + tile.at("file_offset").u64(),
                    bits.data() + offset, count * 8))
      throw std::runtime_error("tile lost edge geometry or physical IEEE bits");
  }
  const auto chunked = directory / "chunked";
  ResourceLimits export_limits;
  export_limits.capacity[ResourceKind::Payload] = 4093;
  ResourceBudget export_budget(export_limits);
  ps::tools::psx::export_results(plan, result, chunked, files, {},
                                 export_budget, 4093);
  if (export_budget.statistics().peak[ResourceKind::Payload] != 4093)
    throw std::runtime_error("export did not respect its staging buffer size");
  if (files.read(chunked / entry.at("path").text()) != file)
    throw std::runtime_error("I/O buffer size changed the output bin");
  ExecutionResult invalid_name;
  invalid_name.results.emplace(std::string("output-") + static_cast<char>(0xff),
                               result.results.at("result"));
  const auto invalid_output = directory / "invalid-name";
  files.reserve(invalid_output);
  bool rejected_name = false;
  try {
    ps::tools::psx::export_results(plan, invalid_name, invalid_output, files,
                                   {}, budget);
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
