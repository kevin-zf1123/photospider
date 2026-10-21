#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include "photospider/compiler/physical_program.hpp"
#include "support/physical_program_fixture.hpp"
#include "support/status_check.hpp"

using namespace ps::physical;                // NOLINT(build/namespaces)
using namespace ps::test::physical_fixture;  // NOLINT(build/namespaces)

int main() {
  auto p = diamond();
  PS_REQUIRE_OK(finalize_program(&p));
  PS_REQUIRE_OK(validate_program(p));
  PS_CHECK(p.tasks[4].predecessors == std::vector<Id>({1, 3}));
  PS_CHECK(p.tasks[0].successors == std::vector<Id>({1}));
  PS_CHECK(p.blocks[0].reservation.outputs[0] == 20);
  PS_CHECK(p.blocks[0].calls == std::vector<Id>({0, 1, 2, 3, 4}));

  // A consumer can precede its producer in the physical task table.
  auto permuted = program(2);
  auto consumer = task(&permuted, 1);
  auto producer = task(&permuted, 0);
  auto source = output(&permuted, producer);
  output(&permuted, consumer);
  permuted.tasks[consumer].reads = {{0, source, 0}};
  PS_REQUIRE_OK(finalize_program(&permuted));
  PS_REQUIRE_OK(validate_program(permuted));
  PS_CHECK(permuted.tasks[0].predecessors == std::vector<Id>({1}));

  // Readiness deduplicates a producer across ports and explicit dependencies.
  auto multi = program(3);
  auto first = task(&multi, 0);
  auto a = output(&multi, first), b = output(&multi, first);
  auto second = task(&multi, 1, {a, a, b});
  multi.tasks[second].explicit_dependencies = {first, first};
  multi.calls[0].returns = {multi.tiles[a].value, multi.tiles[b].value};
  PS_REQUIRE_OK(finalize_program(&multi));
  PS_REQUIRE_OK(validate_program(multi));
  PS_CHECK(multi.tasks[second].predecessors == std::vector<Id>({first}));
  PS_CHECK(multi.tasks[second].reads.size() == 3);
  PS_CHECK(multi.calls[2].tasks.empty());

  for (int error = 0; error < 12; ++error) {
    auto bad = p;
    switch (error) {
      case 0:
        bad.tasks[1].predecessors.clear();
        break;
      case 1:
        bad.tasks[0].successors.clear();
        break;
      case 2:
        bad.tiles[0].producer = 2;
        break;
      case 3:
        bad.tasks[0].writes.push_back(0);
        break;
      case 4:
        bad.blocks[0].calls.push_back(0);
        break;
      case 5:
        bad.calls[0].ordinal = 1;
        break;
      case 6:
        bad.tasks[0].explicit_dependencies = {2};
        break;
      case 7:
        bad.blocks[0].reservation.outputs[0] = 19;
        break;
      case 8:
        bad.tasks[0].placement = none;
        break;
      case 9:
        bad.values[0].schema = none;
        break;
      case 10:
        bad.blocks[0].tasks.pop_back();
        break;
      case 11:
        bad.tiles[0].bytes = 3;
        break;
    }
    PS_CHECK(validate_program(bad).code == ps::ErrorCode::InvalidArgument);
  }
  auto budget = ValidationLimits{};
  budget.maximum_work = 1;
  PS_CHECK(validate_program(p, budget).code ==
           ps::ErrorCode::ResourceExhausted);

  // Derived edges count against the same bound as serialized references.
  auto bounded = diamond();
  budget = {};
  budget.maximum_edges = 31;
  PS_CHECK(finalize_program(&bounded, budget).code ==
           ps::ErrorCode::ResourceExhausted);
  budget = {};
  budget.maximum_records = 1;
  PS_CHECK(validate_program(p, budget).code ==
           ps::ErrorCode::ResourceExhausted);

  // An effect follows every pure preparation, including ordering-only work.
  auto effect = program();
  auto prep = task(&effect, 0);
  auto terminal = task(&effect, 0);
  effect.tasks[terminal].kind = TaskKind::Effect;
  effect.tasks[terminal].explicit_dependencies = {prep};
  effect.calls[0].effect_task = terminal;
  PS_REQUIRE_OK(finalize_program(&effect));
  PS_REQUIRE_OK(validate_program(effect));
  effect.tasks[terminal].explicit_dependencies.clear();
  PS_CHECK(!finalize_program(&effect).ok());
  effect.tasks[terminal].explicit_dependencies = {prep};
  effect.tasks[prep].explicit_dependencies = {terminal};
  PS_CHECK(!finalize_program(&effect).ok());

  // Live-through values survive a block that does not read them.
  auto cfg = program(2);
  cfg.blocks.resize(3);
  cfg.blocks[1].source_block = 2;
  cfg.blocks[2].source_block = 3;
  cfg.blocks[0].calls = {0};
  cfg.blocks[2].calls = {1};
  cfg.calls[1].block = 2;
  cfg.calls[1].ordinal = 0;
  auto carried = output(&cfg, task(&cfg, 0));
  auto final = output(&cfg, task(&cfg, 1, {carried}));
  cfg.blocks[0].end = cfg.blocks[1].end = EndKind::Jump;
  cfg.blocks[0].edges = {{1, {}, {}}};
  cfg.blocks[1].edges = {{2, {}, {}}};
  cfg.return_schemas = {0};
  cfg.blocks[2].return_values = {cfg.tiles[final].value};
  PS_REQUIRE_OK(finalize_program(&cfg));
  PS_REQUIRE_OK(validate_program(cfg));
  PS_CHECK(cfg.blocks[1].live_ins ==
           std::vector<Id>({cfg.tiles[carried].value}));
  PS_CHECK(cfg.blocks[0].edges[0].live_values == cfg.blocks[1].live_ins);
  PS_CHECK(cfg.tasks[1].predecessors.empty());
  cfg.blocks[1].live_ins.clear();
  PS_CHECK(!validate_program(cfg).ok());

  // Parallel edge capture admits swaps and duplicate incoming owners.
  auto loop = program();
  loop.blocks.resize(2);
  loop.blocks[1].source_block = 2;
  auto one = output(&loop, task(&loop, 0));
  auto two = output(&loop, 0);
  auto x = argument(&loop, 1), y = argument(&loop, 1);
  loop.blocks[0].end = loop.blocks[1].end = EndKind::Jump;
  loop.blocks[0].edges = {
      {1, {loop.tiles[one].value, loop.tiles[two].value}, {}}};
  loop.blocks[1].edges = {{1, {y, x}, {}}};
  PS_REQUIRE_OK(finalize_program(&loop));
  PS_REQUIRE_OK(validate_program(loop));
  // Block parameters rename existing allocations; they cannot enlarge or
  // reinterpret the incoming allocation even when the Result schema matches.
  for (int mismatch = 0; mismatch < 3; ++mismatch) {
    auto bad = loop;
    auto tile = bad.values[x].tiles[0];
    if (mismatch == 0) {
      auto strided = bad.layouts[0];
      strided.strides[0] = 2;
      bad.layouts.push_back(strided);
      bad.tiles[tile].layout = static_cast<Id>(bad.layouts.size() - 1);
      bad.tiles[tile].bytes = 7;
    } else if (mismatch == 1) {
      bad.tiles[tile].bytes = 8;
    } else {
      bad.tiles[tile].alignment = 8;
    }
    PS_CHECK(!finalize_program(&bad).ok());
  }
  loop.blocks[1].edges[0].arguments = {x, x};
  PS_REQUIRE_OK(finalize_program(&loop));
  PS_REQUIRE_OK(validate_program(loop));
  for (std::uint64_t work = 1; work < 250; ++work) {
    budget = {};
    budget.maximum_work = work;
    auto status = validate_program(loop, budget);
    PS_CHECK(status.ok() || status.code == ps::ErrorCode::ResourceExhausted);
  }
  loop.blocks[0].edges[0].arguments = {x, y};
  PS_CHECK(!finalize_program(&loop).ok());

  // Device transitions are materialized as COPY, never implicit reads.
  auto copy = program(2);
  auto host = output(&copy, task(&copy, 0));
  auto transfer = task(&copy, 1, {host});
  auto device = output(&copy, transfer);
  copy.placements.push_back({Backend::Gpu, Domain::Device, "gpu", "compute"});
  copy.tasks[transfer].placement = 1;
  copy.tiles[device].domain = Domain::Device;
  PS_CHECK(!finalize_program(&copy).ok());
  copy.tasks[transfer].kind = TaskKind::Copy;
  copy.tasks[transfer].kernel = none;
  copy.tasks[transfer].scratch[2] = 3;
  PS_CHECK(!finalize_program(&copy).ok());
  copy.tasks[transfer].scratch[2] = 4;
  PS_REQUIRE_OK(finalize_program(&copy));
  PS_REQUIRE_OK(validate_program(copy));
  PS_CHECK(copy.blocks[0].reservation.outputs ==
           (std::array<std::uint64_t, 3>{4, 4, 0}));

  auto imported = program(0);
  boundary_import(&imported);
  PS_REQUIRE_OK(finalize_program(&imported));
  PS_CHECK(imported.blocks[0].reservation.outputs[0] == 4);
  for (unsigned error = 0; error < 8; ++error) {
    auto bad = imported;
    switch (error) {
      case 0:
        bad.values[1].import_source = 1;
        break;
      case 1:
        bad.values[1].source_value = 10;
        break;
      case 2:
        bad.values[1].owner = none;
        break;
      case 3:
        bad.tasks[0].kind = TaskKind::Kernel;
        break;
      case 4:
        bad.values[0].input_layout = InputLayout::DirectTiles;
        break;
      case 5:
        bad.tiles[0].bytes = 5;
        break;
      case 6:
        bad.tasks[0].reads[0].tile = 1;
        break;
      case 7:
        bad.values[0].import_source = 0;
        break;
    }
    PS_CHECK(!finalize_program(&bad).ok());
  }
  auto bypass = imported;
  bypass.return_schemas = {0};
  bypass.blocks[0].return_values = {0};
  PS_CHECK(!finalize_program(&bypass).ok());
  auto direct_read = program();
  boundary_import(&direct_read);
  task(&direct_read, 0, {0});
  PS_CHECK(!finalize_program(&direct_read).ok());
  // A real call cannot consume a boundary import scheduled after retirement.
  auto late = program();
  boundary_import(&late);
  task(&late, 0, {1});
  PS_CHECK(!finalize_program(&late).ok());

  Layout view;
  view.shape[0] = 4;
  PS_REQUIRE_OK(validate_layout(view, 4, true));
  view.strides[0] = -1;
  view.byte_offset = 3;
  PS_REQUIRE_OK(validate_layout(view, 4, false));
  PS_CHECK(!validate_layout(view, 4, true).ok());
  view.byte_offset = 2;
  PS_CHECK(!validate_layout(view, 4, false).ok());
  view.strides[0] = 0;
  view.byte_offset = 0;
  view.shape[0] = UINT64_MAX;
  PS_REQUIRE_OK(validate_layout(view, 1, false));
  view.strides[0] = INT64_MIN;
  PS_CHECK(!validate_layout(view, UINT64_MAX, false).ok());
  view.shape[0] = 1;
  PS_REQUIRE_OK(validate_layout(view, 1, false));
  view.origin[0] = UINT64_MAX;
  PS_CHECK(!validate_layout(view, 1, false).ok());
  return 0;
}
