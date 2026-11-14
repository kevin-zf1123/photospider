#include <algorithm>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "photospider/plugin/geometry_mappers.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
ps::OperationDefinition identity() {
  auto d = ps::test::kernel_identity("mapped");
  d.lower = {};
  auto& t = d.tilewise.emplace();
  t.describe = [](const auto&, const auto&) {
    ps::TileTask task;
    task.symbol = "identity";
    task.contract = "scalar-v1";
    return ps::Result<ps::TileTask>(std::move(task));
  };
  return d;
}
int solve_geometry() {
  ps::ResourceBudget budget;
  auto schema = multi_result::schema();
  schema.tensors[0].descriptor.shape = {2, 260, 256};
  auto o = std::make_shared<const ps::SchemaTemplate>(schema);
  auto i = std::make_shared<ps::SchemaTemplate>(schema);
  i->tensors[0].atomic_trailing_axes = 1;
  for (uint64_t extent : {1U, 128U}) {
    auto ig = ps::TileGrid::from_schema(*i, extent);
    PS_REQUIRE_OK(ig);
    auto og = ps::TileGrid::from_schema(*o, extent);
    PS_REQUIRE_OK(og);
    auto region = og.value().region(og.value().tile_count() - 1);
    PS_REQUIRE_OK(region);
    ps::MappingQuery q;
    q.region = region.value();
    q.sources.push_back({3, 0, ig.value(), {i}});
    ps::MappingSink sink(q, budget, ps::physical::ValidationLimits{});
    PS_REQUIRE_OK(sink.region(0, q.region));
    PS_REQUIRE_OK(sink.descriptor(0));
    PS_REQUIRE_OK(sink.region(0, q.region));
    auto needs = sink.take();
    PS_CHECK(needs.size() == 3 && needs[0].input == 3 &&
             needs[1].descriptor_only && needs[0].tiles == needs[2].tiles);
    auto bad =
        sink.interval(0, ig.value().tile_count(), ig.value().tile_count() + 1);
    PS_CHECK(!bad.ok());
    PS_CHECK(!sink.consume(1).ok());
  }
  return 0;
}
using GeometryReads = std::vector<std::pair<std::uint32_t, std::uint64_t>>;
int geometry_case(const char* contract, std::vector<std::uint64_t> config,
                  const ps::SchemaTemplate& schema, std::uint64_t extent,
                  ps::TileIndex tile, const GeometryReads& expected) {
  ps::OperationRegistry registry;
  PS_REQUIRE_OK(registry.load_plugin(PS_GEOMETRY_MODULE));
  auto operation = identity();
  operation.traits.input_schema[0] = {};
  operation.traits.input_schema[0].result_schema_id = schema.id;
  operation.traits.input_schema[0].result_schema_version = schema.version;
  operation.traits.outputs[0].output_schema = operation.traits.input_schema[0];
  operation.tilewise->external = "support";
  operation.mapping_dependencies.push_back({"support", contract, "geometry"});
  operation.tilewise->configuration = [config =
                                           std::move(config)](const auto&) {
    return ps::Result<std::vector<std::uint64_t>>(config);
  };
  PS_REQUIRE_OK(registry.register_operation(std::move(operation)));
  PS_REQUIRE_OK(registry.freeze());
  auto input = std::make_shared<const ps::SchemaTemplate>(schema);
  auto prepared = registry.prepare_operation("mapped", {{input}}, {});
  PS_REQUIRE_OK(prepared);
  ps::TileRequest selection;
  selection.members.emplace(0, multi_result::take(ps::TileSet::range(
                                   ps::ResourceBudget{}, tile, tile + 1)));
  auto lowered = prepared.value()->lower({selection}, extent,
                                         ps::physical::Backend::Cpu, {});
  PS_REQUIRE_OK(lowered);
  const auto& program = lowered.value().program;
  PS_CHECK(program.tasks.size() == 1);
  GeometryReads reads;
  for (const auto& read : program.tasks[0].reads) {
    const auto& source = program.tiles.at(read.tile);
    reads.emplace_back(source.member, source.tile_index);
  }
  std::sort(reads.begin(), reads.end());
  PS_CHECK(reads == expected);
  return 0;
}
int external_geometry() {
  auto schema = multi_result::schema(ps::ElementType::Float64, {2, 5, 7});
  for (std::uint64_t border : {0U, 1U, 2U, 3U, 4U, 5U}) {
    GeometryReads expected{{0, 35}, {0, 36}};
    if (border == 2)
      expected = {{0, 35}, {0, 36}, {0, 40}, {0, 41},
                  {0, 63}, {0, 64}, {0, 68}, {0, 69}};
    else if (border == 4)
      expected = {{0, 35}, {0, 36}, {0, 37}, {0, 42}, {0, 43}, {0, 44}};
    const std::vector<std::uint64_t> config{1, 2, 1, 0, 2, 1, border};
    PS_CHECK(geometry_case("halo", config, schema, 1, 35, expected) == 0);
    PS_CHECK(geometry_case("halo", config, schema, 128, 1, {{0, 1}}) == 0);
  }
  auto member = schema.tensors[0];
  member.key = "half";
  member.descriptor.shape = {2, 3, 4};
  schema.tensors.push_back(member);
  member.key = "quarter";
  member.descriptor.shape = {2, 2, 2};
  schema.tensors.push_back(member);
  GeometryReads plane;
  for (std::uint32_t slot = 0; slot < 3; ++slot) {
    const auto& shape = schema.tensors[slot].descriptor.shape;
    const auto area = shape[1] * shape[2];
    for (auto tile = area; tile < 2 * area; ++tile)
      plane.emplace_back(slot, tile);
  }
  PS_CHECK(geometry_case("plane", {1, 2}, schema, 1, 35, plane) == 0);
  PS_CHECK(geometry_case("plane", {1, 2}, schema, 128, 1,
                         {{0, 1}, {1, 1}, {2, 1}}) == 0);
  return 0;
}
int dispatch() {
  ps::OperationRegistry registry;
  auto d = identity();
  d.tilewise->external = "stage";
  d.mapping_dependencies.push_back({"stage", "select", "test"});
  PS_REQUIRE_OK(registry.register_operation(d));
  auto invalid = d;
  invalid.key = "invalid";
  invalid.lower = ps::test::kernel_identity("a").lower;
  PS_CHECK(!registry.register_operation(invalid).ok());
  PS_REQUIRE_OK(registry.load_plugin(PS_ERROR_MAPPER));
  auto broken = identity();
  broken.key = "ignored-sink";
  broken.tilewise->external = "bad";
  broken.mapping_dependencies.push_back({"bad", "bad-sink", "fixture"});
  PS_REQUIRE_OK(registry.register_operation(std::move(broken)));
  auto direct = identity();
  direct.key = "direct";
  PS_REQUIRE_OK(registry.register_operation(direct));
  auto local = identity();
  local.key = "bad-local";
  local.tilewise->local = [](const auto&, ps::MappingSink& sink) {
    (void)sink.descriptor(999);
    return ps::Status{};
  };
  PS_REQUIRE_OK(registry.register_operation(local));
  auto custom = ps::test::kernel_identity("temporary");
  custom.mapping_dependencies = d.mapping_dependencies;
  custom.lower = [](const ps::OperationLowering& q) {
    ps::OperationProgramBuilder b(q);
    auto schema = q.inputs[0].result_schema;
    auto grid =
        multi_result::take(ps::TileGrid::from_schema(*schema, q.tile_extent));
    auto first = multi_result::take(b.add_copy());
    multi_result::check(b.read_input(first, 0, 0, 0));
    auto val = multi_result::take(b.add_temporary(schema));
    auto ti = multi_result::take(b.write_temporary(first, val, 0, 0));
    ps::MappingQuery m;
    m.tile_extent = q.tile_extent;
    m.region = multi_result::take(grid.region(0));
    m.sources.push_back({0, 0, grid, {schema}});
    auto needs = q.map("stage", m);
    if (!needs.ok())
      return ps::Result<ps::OperationProgram>(needs.status());
    auto last = multi_result::take(b.add_copy());
    for (const auto& n : needs.value())
      for (auto index : n.tiles) {
        if (index != 0)
          return ps::Result<ps::OperationProgram>(
              {ps::ErrorCode::InvalidArgument, "wrong private tile"});
        multi_result::check(b.read_tile(last, ti));
      }
    multi_result::check(b.write_output(last, 0, 0, 0));
    return b.finish();
  };
  PS_REQUIRE_OK(registry.register_operation(custom));
  std::atomic<unsigned> calls{0};
  PS_REQUIRE_OK(registry.register_mapper(
      {"select", "test", 1, 1, [&](const auto& q, auto& sink) {
         ++calls;
         return sink.region(0, q.region);
       }}));
  PS_REQUIRE_OK(registry.freeze());
  auto schema =
      std::make_shared<const ps::SchemaTemplate>(multi_result::schema());
  ps::TileRequest selection;
  selection.members.emplace(
      0, multi_result::take(ps::TileSet::range(ps::ResourceBudget{}, 0, 1)));
  auto lower = [&](const char* name, uint64_t work = 10000) {
    auto op =
        multi_result::take(registry.prepare_operation(name, {{schema}}, {}));
    return op->lower({selection}, 1, ps::physical::Backend::Cpu,
                     {1024, 1024, work});
  };
  auto future = std::async(std::launch::async, [&] { return lower("mapped"); });
  auto mapped = lower("mapped");
  PS_REQUIRE_OK(mapped);
  PS_REQUIRE_OK(future.get());
  auto direct_plan = lower("direct");
  PS_REQUIRE_OK(direct_plan);
  PS_CHECK(mapped.value().program.tasks[0].reads.size() ==
           direct_plan.value().program.tasks[0].reads.size());
  auto staged = lower("temporary");
  PS_REQUIRE_OK(staged);
  PS_CHECK(staged.value().program.tasks.size() == 2 &&
           staged.value().program.tasks.back().predecessors.size() == 1);
  PS_CHECK(calls == 3 && !lower("bad-local").ok());
  PS_CHECK(!lower("ignored-sink").ok());
  ps::physical::ValidationLimits limits;
  ps::LoweringSession session(limits);
  auto op =
      multi_result::take(registry.prepare_operation("mapped", {{schema}}, {}));
  auto count = calls.load();
  PS_REQUIRE_OK(
      op->lower({selection}, 1, ps::physical::Backend::Cpu, limits, &session));
  PS_REQUIRE_OK(
      op->lower({selection}, 1, ps::physical::Backend::Cpu, limits, &session));
  PS_CHECK(calls == count + 1);
  auto exhausted = lower("mapped", 1);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ps::ErrorCode::ResourceExhausted);
  return 0;
}
// A separate runtime catalog must be sufficient even when the saved
// operation still declares an external compiler-only dependency.
int artifact_without_mapper() {
  auto make_sum = []() {
    ps::OperationDefinition d;
    d.key = "math.add";
    d.traits.input_count = 2;
    d.traits.input_schema.resize(2);
    for (auto& input : d.traits.input_schema) {
      input.result_schema_id = "test.multi_output";
      input.result_schema_version = 1;
    }
    d.traits.outputs[0] = multi_result::output("value");
    ps::test::install_scalar(d, true);
    d.lower = {};
    d.mapping_dependencies.push_back({"both", "point", "test"});
    auto& t = d.tilewise.emplace();
    t.external = "both";
    t.describe = [](const auto&, const auto&) {
      ps::TileTask task;
      task.symbol = "sum";
      task.contract = "scalar-v1";
      return ps::Result<ps::TileTask>(std::move(task));
    };
    return d;
  };
  auto registry = [&] {
    auto ops = std::make_shared<ps::OperationRegistry>();
    ps::OperationDefinition constant;
    constant.key = "core.constant";
    constant.traits.outputs[0] = multi_result::output("value");
    constant.traits.parameter_schema = {
        {"value", ps::OperationParameterType::Float64, true}};
    ps::test::install_scalar(constant, false);
    multi_result::check(ops->register_operation(std::move(constant)));
    multi_result::check(ops->register_operation(make_sum()));
    return ops;
  };
  int solved = 0;
  std::string artifact;
  {
    auto ops = registry();
    PS_REQUIRE_OK(ops->register_mapper(
        {"point", "test", 1, 1, [&](const auto& q, ps::MappingSink& sink) {
           ++solved;
           for (unsigned slot = 0; slot < q.sources.size(); ++slot) {
             auto s = sink.region(slot, q.region);
             if (!s.ok())
               return s;
           }
           return ps::Status{};
         }}));
    PS_REQUIRE_OK(ops->freeze());
    ps::Compiler compiler(ops);
    ps::GraphContext graph(ps::test::addition_document(4, 5));
    auto compiled = compiler.compile(graph);
    PS_REQUIRE_OK(compiled);
    artifact = multi_result::take(
        ps::CompilerArtifact::export_plan(*compiled.value().plan));
  }
  const auto count = solved;
  PS_CHECK(count > 0);
  auto runtime = registry();
  PS_REQUIRE_OK(runtime->freeze());
  auto imported = ps::CompilerArtifact::import_plan(runtime, artifact);
  PS_REQUIRE_OK(imported);
  auto selected =
      imported.value().select_outputs(imported.value().output_requests());
  PS_REQUIRE_OK(selected);
  ps::ExecutionContext context(runtime);
  auto result = multi_result::run(context, selected.value());
  PS_REQUIRE_OK(result);
  PS_CHECK(ps::test::named_scalar(result.value(), "sum") == 9 &&
           solved == count);
  ps::GraphContext graph(ps::test::addition_document(4, 5));
  auto missing = ps::Compiler(runtime).compile(graph);
  PS_CHECK(!missing.ok() && missing.status().code == ps::ErrorCode::NotFound);
  return 0;
}
}  // namespace
int main() {
  return solve_geometry() || external_geometry() || dispatch() ||
         artifact_without_mapper();
}
