#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "photospider/compiler/physical_program.hpp"

namespace ps::test::physical_fixture {
using namespace ps::physical;  // NOLINT(build/namespaces)

inline Program program(Id calls = 1) {
  Program p;
  auto schema = std::make_shared<ps::SchemaTemplate>();
  schema->id = "physical.test";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::UInt8, {4}};
  schema->tensors.push_back(std::move(tensor));
  p.schemas.push_back(std::move(schema));
  Layout layout;
  layout.shape[0] = 4;
  p.layouts.push_back(layout);
  p.placements.push_back({});
  p.kernels.push_back({"test", "compute", "test-v1"});
  p.blocks.resize(1);
  p.blocks[0].source_block = 1;
  for (Id i = 0; i < calls; ++i) {
    CallDesc call;
    call.source_call = i + 1;
    call.block = 0;
    call.ordinal = i;
    p.calls.push_back(call);
    p.blocks[0].calls.push_back(i);
  }
  return p;
}

inline Id task(Program* p, Id call, const std::vector<Id>& reads = {}) {
  Id ti = static_cast<Id>(p->tasks.size());
  TaskDesc desc;
  desc.call = call;
  desc.kernel = 0;
  desc.placement = 0;
  for (auto tile : reads)
    desc.reads.push_back({0, tile, 0});
  p->tasks.push_back(desc);
  p->calls[call].tasks.push_back(ti);
  p->blocks[p->calls[call].block].tasks.push_back(ti);
  return ti;
}

inline Id output(Program* p, Id ti) {
  Id vi = static_cast<Id>(p->values.size());
  Id tile = static_cast<Id>(p->tiles.size());
  ValueDesc value;
  value.source_value = vi + 1;
  value.schema = 0;
  value.owner = p->tasks[ti].call;
  value.tiles.push_back(tile);
  p->values.push_back(value);
  TileDesc desc;
  desc.value = vi;
  desc.layout = 0;
  desc.producer = ti;
  desc.bytes = 4;
  p->tiles.push_back(desc);
  p->tasks[ti].writes.push_back(tile);
  return tile;
}

inline Id argument(Program* p, Id bi) {
  Id vi = static_cast<Id>(p->values.size());
  Id tile = static_cast<Id>(p->tiles.size());
  ValueDesc value;
  value.source_value = vi + 1;
  value.schema = 0;
  value.owner = bi;
  value.kind = ValueKind::Argument;
  value.tiles.push_back(tile);
  p->values.push_back(value);
  TileDesc desc;
  desc.value = vi;
  desc.layout = 0;
  desc.bytes = 4;
  p->tiles.push_back(desc);
  p->blocks[bi].arguments.push_back(vi);
  return vi;
}

inline Id boundary_import(Program* p, Id block = 0) {
  Id raw_value = static_cast<Id>(p->values.size());
  Id raw_tile = static_cast<Id>(p->tiles.size());
  Id ti = static_cast<Id>(p->tasks.size());
  ValueDesc input;
  input.kind = ValueKind::Input;
  input.schema = 0;
  input.owner = static_cast<Id>(p->inputs.size());
  input.input_layout = InputLayout::DenseHost;
  input.tiles = {raw_tile};
  p->inputs.push_back(raw_value);
  p->values.push_back(input);
  TileDesc raw;
  raw.value = raw_value;
  raw.layout = 0;
  raw.bytes = 4;
  p->tiles.push_back(raw);
  ValueDesc imported;
  imported.kind = ValueKind::Imported;
  imported.schema = 0;
  imported.owner = block;
  imported.import_source = raw_value;
  imported.tiles = {raw_tile + 1};
  p->values.push_back(imported);
  raw.value = raw_value + 1;
  raw.producer = ti;
  p->tiles.push_back(raw);
  TaskDesc copy;
  copy.kind = TaskKind::Copy;
  copy.placement = 0;
  copy.reads = {{0, raw_tile, 0}};
  copy.writes = {raw_tile + 1};
  p->tasks.push_back(copy);
  p->blocks[block].tasks.push_back(ti);
  return ti;
}

inline Program diamond() {
  auto p = program(5);
  auto a = output(&p, task(&p, 0));
  auto c = output(&p, task(&p, 1, {a}));
  auto b = output(&p, task(&p, 2));
  auto d = output(&p, task(&p, 3, {b}));
  auto e = output(&p, task(&p, 4, {c, d}));
  p.return_schemas = {0};
  p.blocks[0].return_values = {p.tiles[e].value};
  return p;
}
}  // namespace ps::test::physical_fixture
