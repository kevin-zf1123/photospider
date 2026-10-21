#include "photospider/plugin/operation_program.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
Status limit() {
  return {ErrorCode::ResourceExhausted, "operation subgraph limit"};
}
}  // namespace
bool OperationProgramBuilder::admit(std::uint64_t records,
                                    std::uint64_t references) {
  if (!status_.ok())
    return false;
  const auto& p = result_.program;
  std::uint64_t remaining = query_.limits.maximum_records;
  for (auto size : {p.schemas.size(), p.layouts.size(), p.tiles.size(),
                    p.values.size(), p.placements.size(), p.kernels.size(),
                    p.tasks.size(), p.calls.size(), p.blocks.size()}) {
    if (size > remaining) {
      status_ = limit();
      return false;
    }
    remaining -= size;
  }
  if (records > remaining || references_ > query_.limits.maximum_edges ||
      references > query_.limits.maximum_edges - references_) {
    status_ = limit();
    return false;
  }
  references_ += references;
  return true;
}
OperationProgramBuilder::OperationProgramBuilder(const OperationLowering& query)
    : query_(query) {
  auto& p = result_.program;
  p.tile_extent = query.tile_extent;
  p.blocks.resize(1);
  p.blocks[0].source_block = 1;
  p.blocks[0].calls = {0};
  p.calls.resize(1);
  p.calls[0].source_call = 1;
  p.calls[0].block = 0;
  if (!admit(0, 1))
    return;
  if (query.inputs.size() > 1024 || query.outputs.empty() ||
      query.outputs.size() > 64 ||
      query.requests.size() != query.outputs.size()) {
    status_ = invalid("invalid operation lowering signature");
    return;
  }
  auto value = [&](const OperationMetadata& metadata, bool input) {
    if (!status_.ok())
      return;
    if (!metadata.result_schema) {
      status_ = invalid("missing operation lowering schema");
      return;
    }
    auto grids = TileGrid::members(*metadata.result_schema, query.tile_extent);
    if (!grids.ok()) {
      status_ = grids.status();
      return;
    }
    if (!admit(2, input ? 1 : 2))
      return;
    physical::ValueDesc v;
    v.schema = static_cast<physical::Id>(p.schemas.size());
    v.kind = input ? physical::ValueKind::Input : physical::ValueKind::Produced;
    v.owner = input ? static_cast<physical::Id>(p.inputs.size()) : 0;
    auto vi = static_cast<physical::Id>(p.values.size());
    (input ? p.inputs : result_.outputs).push_back(vi);
    if (!input) {
      p.return_schemas.push_back(v.schema);
      p.blocks[0].return_values.push_back(vi);
    }
    p.schemas.push_back(metadata.result_schema);
    grids_.push_back(grids.take_value());
    p.values.push_back(v);
  };
  for (const auto& input : query.inputs)
    value(input, true);
  for (const auto& output : query.outputs)
    value(output, false);
}

Result<physical::Id> OperationProgramBuilder::add_task(
    const std::string& symbol, const std::string& contract,
    physical::Placement placement, std::array<std::uint64_t, 3> scratch,
    bool effect) {
  using Answer = Result<physical::Id>;
  if (!status_.ok())
    return Answer(status_);
  auto& p = result_.program;
  if (p.tasks.size() >= query_.limits.maximum_records ||
      p.tasks.size() >= physical::none)
    return Answer(limit());
  if ((query_.backend == physical::Backend::Cpu &&
       placement.backend != physical::Backend::Cpu) ||
      symbol.empty() || contract.empty() ||
      (effect && p.calls[0].effect_task != physical::none))
    return Answer(invalid("invalid operation kernel or effect"));
  physical::TaskDesc task;
  task.call = 0;
  task.kernel = static_cast<physical::Id>(p.kernels.size());
  task.placement = static_cast<physical::Id>(p.placements.size());
  task.scratch = scratch;
  task.kind = effect ? physical::TaskKind::Effect : physical::TaskKind::Kernel;
  auto ti = static_cast<physical::Id>(p.tasks.size());
  if (!admit(3, 2))
    return Answer(status_);
  p.tasks.push_back(task);
  p.kernels.push_back({query_.operation, symbol, contract});
  p.placements.push_back(std::move(placement));
  p.calls[0].tasks.push_back(ti);
  p.blocks[0].tasks.push_back(ti);
  if (effect)
    p.calls[0].effect_task = ti;
  return Answer(ti);
}

Result<physical::Id> OperationProgramBuilder::add_copy(
    physical::Placement placement) {
  using Answer = Result<physical::Id>;
  if (!status_.ok())
    return Answer(status_);
  auto& p = result_.program;
  if (p.tasks.size() >= query_.limits.maximum_records ||
      p.tasks.size() >= physical::none)
    return Answer(limit());
  physical::TaskDesc task;
  task.kind = physical::TaskKind::Copy;
  task.call = 0;
  task.placement = static_cast<physical::Id>(p.placements.size());
  const auto ti = static_cast<physical::Id>(p.tasks.size());
  if (!admit(2, 2))
    return Answer(status_);
  p.tasks.push_back(std::move(task));
  p.placements.push_back(std::move(placement));
  p.calls[0].tasks.push_back(ti);
  p.blocks[0].tasks.push_back(ti);
  return Answer(ti);
}

Result<physical::Id> OperationProgramBuilder::add_temporary(
    std::shared_ptr<const SchemaTemplate> schema) {
  using Answer = Result<physical::Id>;
  if (!status_.ok())
    return Answer(status_);
  auto& p = result_.program;
  if (!schema)
    return Answer(invalid("null temporary schema"));
  if (p.values.size() >= query_.limits.maximum_records ||
      p.values.size() >= physical::none)
    return Answer(limit());
  if (!admit(2, 0))
    return Answer(status_);
  auto grids = TileGrid::members(*schema, query_.tile_extent);
  if (!grids.ok())
    return Answer(grids.status());
  physical::ValueDesc value;
  value.schema = static_cast<physical::Id>(p.schemas.size());
  value.owner = 0;
  auto vi = static_cast<physical::Id>(p.values.size());
  p.schemas.push_back(std::move(schema));
  grids_.push_back(grids.take_value());
  p.values.push_back(value);
  return Answer(vi);
}

Result<physical::Id> OperationProgramBuilder::tile(physical::Id value,
                                                   std::uint32_t member,
                                                   TileIndex index,
                                                   physical::Domain domain,
                                                   physical::Id producer) {
  using Answer = Result<physical::Id>;
  auto& p = result_.program;
  if (value >= p.values.size() || member >= grids_[value].size())
    return Answer(invalid("invalid operation tile address"));
  auto key = std::make_tuple(value, member, index);
  auto existing = tiles_.find(key);
  if (existing != tiles_.end()) {
    const auto& desc = p.tiles[existing->second];
    if (desc.domain != domain || producer != physical::none)
      return Answer(invalid("repeated tile writer or conflicting domain"));
    return Answer(existing->second);
  }
  if (p.tiles.size() >= query_.limits.maximum_records ||
      p.tiles.size() >= physical::none)
    return Answer(limit());
  auto region = grids_[value][member].region(index);
  if (!region.ok())
    return Answer(region.status());
  const auto& schema = *p.schemas[p.values[value].schema];
  physical::Layout layout;
  layout.element_type = schema.tensors[member].descriptor.element_type;
  layout.rank = static_cast<std::uint32_t>(region.value().rank());
  layout.strides.fill(0);
  std::uint64_t bytes = TensorSpan::element_size(layout.element_type);
  const auto alignment = bytes;
  for (unsigned axis = layout.rank; axis-- > 0;) {
    const auto d = region.value().dimensions()[axis];
    if (bytes > INT64_MAX || d.extent > UINT64_MAX / bytes)
      return Answer(limit());
    layout.strides[axis] = static_cast<std::int64_t>(bytes);
    layout.shape[axis] = d.extent;
    layout.origin[axis] = d.offset;
    bytes *= d.extent;
  }
  physical::TileDesc desc;
  desc.value = value;
  desc.member = member;
  desc.tile_index = index;
  desc.producer = producer;
  desc.domain = domain;
  desc.bytes = bytes;
  desc.alignment = alignment;
  desc.layout = static_cast<physical::Id>(p.layouts.size());
  auto ti = static_cast<physical::Id>(p.tiles.size());
  if (!admit(2, 1))
    return Answer(status_);
  p.layouts.push_back(layout);
  p.tiles.push_back(desc);
  p.values[value].tiles.push_back(ti);
  tiles_.emplace(key, ti);
  return Answer(ti);
}

Status OperationProgramBuilder::read_input(physical::Id task,
                                           std::uint32_t port,
                                           std::uint32_t member,
                                           TileIndex index,
                                           physical::Domain domain) {
  if (!status_.ok())
    return status_;
  auto& p = result_.program;
  if (task >= p.tasks.size() || port >= p.inputs.size())
    return invalid("invalid operation input read");
  auto input = tile(p.inputs[port], member, index, domain, physical::none);
  if (!input.ok())
    return input.status();
  return read_tile(task, input.value(), port);
}
Status OperationProgramBuilder::read_input_schema(physical::Id task,
                                                  std::uint32_t port) {
  if (!status_.ok())
    return status_;
  auto& p = result_.program;
  if (task >= p.tasks.size() || port >= p.inputs.size())
    return invalid("invalid operation schema read");
  auto& reads = p.tasks[task].schema_reads;
  if (std::find(reads.begin(), reads.end(), p.inputs[port]) == reads.end()) {
    if (!admit(0, 1))
      return status_;
    reads.push_back(p.inputs[port]);
  }
  return Status::success();
}
Status OperationProgramBuilder::write_output(physical::Id task,
                                             std::uint32_t output,
                                             std::uint32_t member,
                                             TileIndex index) {
  if (output >= result_.outputs.size())
    return invalid("invalid output port");
  auto written = write_temporary(task, result_.outputs[output], member, index);
  return written.status();
}
Result<physical::Id> OperationProgramBuilder::write_temporary(
    physical::Id task, physical::Id value, std::uint32_t member,
    TileIndex index) {
  using Answer = Result<physical::Id>;
  if (!status_.ok())
    return Answer(status_);
  auto& p = result_.program;
  if (task >= p.tasks.size() || value >= p.values.size() ||
      p.values[value].kind != physical::ValueKind::Produced)
    return Answer(invalid("invalid operation output write"));
  const auto domain = p.placements[p.tasks[task].placement].domain;
  auto result = tile(value, member, index, domain, task);
  if (result.ok()) {
    if (!admit(0, 1))
      return Answer(status_);
    p.tasks[task].writes.push_back(result.value());
  }
  return result;
}
Status OperationProgramBuilder::read_tile(physical::Id task, physical::Id tile,
                                          std::uint32_t port) {
  auto& p = result_.program;
  if (task >= p.tasks.size() || tile >= p.tiles.size())
    return invalid("invalid tile read");
  if (!admit(0, 1))
    return status_;
  p.tasks[task].reads.push_back({port, tile, p.tiles[tile].layout});
  return Status::success();
}
Status OperationProgramBuilder::depends_on(physical::Id task,
                                           physical::Id predecessor) {
  auto& p = result_.program;
  if (task >= p.tasks.size() || predecessor >= p.tasks.size())
    return invalid("invalid explicit operation dependency");
  if (!admit(0, 1))
    return status_;
  p.tasks[task].explicit_dependencies.push_back(predecessor);
  return Status::success();
}
std::vector<std::uint8_t>& OperationProgramBuilder::parameters(
    physical::Id task) {
  return result_.program.tasks.at(task).parameters;
}
Result<OperationProgram> OperationProgramBuilder::finish() {
  using Answer = Result<OperationProgram>;
  if (!status_.ok())
    return Answer(status_);
  auto& p = result_.program;
  for (auto& value : p.values)
    std::sort(value.tiles.begin(), value.tiles.end(), [&](auto a, auto b) {
      return std::tie(p.tiles[a].member, p.tiles[a].tile_index) <
             std::tie(p.tiles[b].member, p.tiles[b].tile_index);
    });
  for (std::size_t oi = 0; oi < result_.outputs.size(); ++oi) {
    auto vi = result_.outputs[oi];
    auto request = query_.requests[oi].resolve(grids_[vi]);
    if (!request.ok())
      return Answer(request.status());
    if (request.value().size() > query_.limits.maximum_records)
      return Answer(limit());
    for (const auto& member : request.value().members)
      for (auto index : member.second)
        if (!tiles_.count({vi, member.first, index}))
          return Answer(invalid("operation omits a requested output tile"));
  }
  for (auto& task : p.tasks) {
    if (task.kind != physical::TaskKind::Copy ||
        p.placements[task.placement].backend != physical::Backend::Gpu ||
        task.reads.size() != 1 || task.writes.size() != 1)
      continue;
    const auto host_ends =
        static_cast<std::uint64_t>(p.tiles[task.reads[0].tile].domain ==
                                   physical::Domain::Host) +
        static_cast<std::uint64_t>(p.tiles[task.writes[0]].domain ==
                                   physical::Domain::Host);
    const auto bytes = p.tiles[task.writes[0]].bytes;
    if (host_ends && bytes > UINT64_MAX / host_ends)
      return Answer(limit());
    task.scratch[static_cast<unsigned>(physical::Domain::Shared)] =
        std::max(task.scratch[static_cast<unsigned>(physical::Domain::Shared)],
                 bytes * host_ends);
  }
  auto status = physical::finalize_program(&p, query_.limits);
  if (!status.ok())
    return Answer(status);
  status_ = invalid("operation program builder has been consumed");
  return Answer(std::move(result_));
}
}  // namespace ps
