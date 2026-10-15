#include "execution/tile_cache.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"

namespace ps::execution_internal {
namespace {
Status mismatch() {
  return {ErrorCode::NotFound,
          "cached tile routes do not match the current plan"};
}
bool same_input(const PlanInput& a, const PlanInput& b) {
  if (a.index() != b.index())
    return false;
  if (const auto* step = std::get_if<PlanStepInput>(&a))
    return step->step_index == std::get<PlanStepInput>(b).step_index;
  return std::get<PlanWorkflowInput>(a).declaration_index ==
         std::get<PlanWorkflowInput>(b).declaration_index;
}
Result<TileRecords::ProducerUnits> producer_units(const ExecutionPlan& plan,
                                                  const PlanStepInput& input,
                                                  std::uint32_t output) {
  if (input.step_index >= plan.steps().size())
    return Result<TileRecords::ProducerUnits>(mismatch());
  const auto& step = plan.steps()[input.step_index];
  if (step.output_index != output || !step.output_result_schema)
    return Result<TileRecords::ProducerUnits>(mismatch());
  const auto& fields = step.output_result_schema->fields;
  const bool dynamic =
      std::any_of(fields.begin(), fields.end(), [](const auto& field) {
        return field.rows.kind == ResultExtentKind::RuntimeCount;
      });
  return Result<TileRecords::ProducerUnits>(TileRecords::ProducerUnits{
      step.tiled ? step.tile_grid->tile_count() : 1, dynamic});
}
}  // namespace
Result<std::shared_ptr<const TileCacheSnapshot>> TileCacheSnapshot::capture(
    const ResourceBudget& root, const ExecutionPlan& plan,
    const std::vector<std::string>& contracts, const TileRecords& table,
    const TileRecord& task, std::uint64_t maximum_metadata,
    const std::function<Status(std::uint64_t)>& charge) try {
  using Answer = Result<std::shared_ptr<const TileCacheSnapshot>>;
  auto result = std::allocate_shared<TileCacheSnapshot>(
      ResourceAllocator<TileCacheSnapshot>(root), root);
  const auto account = [&](std::uint64_t n) -> Status {
    if (!core_internal::can_add(result->metadata, n, maximum_metadata))
      return {ErrorCode::ResourceExhausted, "tile cache metadata limit"};
    auto status = charge(n);
    if (status.ok())
      result->metadata += n;
    return status;
  };
  auto status = account(1 + task.inputs.size());
  if (!status.ok())
    return Answer(status);
  auto root_record = std::allocate_shared<TileRecord>(
      ResourceAllocator<TileRecord>(root), root);
  root_record->unit = task.unit;
  root_record->computed_descriptor = task.computed_descriptor;
  for (const auto& dependency : task.inputs) {
    status = account(dependency.tiles.size());
    if (status.ok())
      status = root_record->include(dependency, root);
    if (!status.ok())
      return Answer(status);
  }
  auto ancestry = table.capture_ancestry(
      task,
      [&](const PlanStepInput& input, std::uint32_t output) {
        return producer_units(plan, input, output);
      },
      charge);
  if (!ancestry.ok())
    return Answer(ancestry.status());
  auto closure = ancestry.take_value();
  closure.insert(closure.begin(), std::move(root_record));
  TileRecord source_union(root);
  for (const auto& record : closure) {
    if (record->unit.step >= plan.steps().size() ||
        record->unit.step >= contracts.size())
      return Answer(mismatch());
    const auto& step = plan.steps()[record->unit.step];
    status = account(1 + record->inputs.size() +
                     contracts[record->unit.step].size());
    if (!status.ok())
      return Answer(status);
    TileCacheRecord routed{record,
                           ResourceString(contracts[record->unit.step],
                                          ResourceAllocator<char>(root)),
                           ResourceVector<TileCacheRoute>(
                               ResourceAllocator<TileCacheRoute>(root))};
    const auto& projection = step.traits.outputs[0].input_indices;
    for (const auto& dependency : record->inputs) {
      status = account(1 + dependency.tiles.size());
      if (status.ok())
        status = charge(step.inputs.size() *
                        (1 + (projection ? projection->size() : 0)));
      if (!status.ok())
        return Answer(status);
      TileCacheRoute route{
          ResourceVector<std::uint32_t>(ResourceAllocator<std::uint32_t>(root)),
          ResourceString(ResourceAllocator<char>(root)),
          ResourceString(ResourceAllocator<char>(root))};
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        if (projection && std::find(projection->begin(), projection->end(),
                                    port) == projection->end())
          continue;
        if (same_input(step.inputs[port], dependency.producer))
          route.ports.push_back(port);
      }
      if (route.ports.empty())
        return Answer(mismatch());
      status = account(route.ports.size());
      if (!status.ok())
        return Answer(status);
      if (const auto* producer =
              std::get_if<PlanStepInput>(&dependency.producer)) {
        if (producer->step_index >= contracts.size())
          return Answer(mismatch());
        status = account(contracts[producer->step_index].size());
        if (!status.ok())
          return Answer(status);
        route.contract.assign(contracts[producer->step_index]);
      } else {
        const auto input =
            std::get<PlanWorkflowInput>(dependency.producer).declaration_index;
        if (input >= plan.input_declarations().size())
          return Answer(mismatch());
        const auto& name = plan.input_declarations()[input].name;
        status = account(name.size());
        if (status.ok())
          status = source_union.include(dependency, root);
        if (!status.ok())
          return Answer(status);
        route.source.assign(name);
      }
      routed.routes.push_back(std::move(route));
    }
    result->records.push_back(std::move(routed));
  }
  for (const auto& source : source_union.inputs) {
    const auto input =
        std::get<PlanWorkflowInput>(source.producer).declaration_index;
    const auto& name = plan.input_declarations()[input].name;
    status = account(1 + name.size() + source.tiles.size());
    if (!status.ok())
      return Answer(status);
    result->sources.push_back(
        {ResourceString(name, ResourceAllocator<char>(root)), source.whole,
         source.tiles});
  }
  // Names, not declaration positions, define portable source ordering.
  std::uint64_t levels = 1;
  for (auto n = result->sources.size(); n; n >>= 1)
    ++levels;
  std::uint64_t name_work = 1;
  for (const auto& source : result->sources)
    name_work = std::max<std::uint64_t>(name_work, source.name.size() + 1);
  if (!core_internal::can_multiply(levels, result->sources.size()) ||
      !core_internal::can_multiply(levels * result->sources.size(), name_work))
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  status = charge(levels * result->sources.size() * name_work);
  if (!status.ok())
    return Answer(status);
  std::sort(result->sources.begin(), result->sources.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<std::shared_ptr<const TileCacheSnapshot>>(
      Status{ErrorCode::ResourceExhausted, {}});
}
Result<ReboundTileCache> TileCacheSnapshot::rebind(
    const ResourceBudget& root, const ExecutionPlan& plan,
    const std::vector<std::string>& contracts, TileUnit task,
    const std::function<Status(std::uint64_t)>& charge) const try {
  using Answer = Result<ReboundTileCache>;
  if (records.empty() || task.output != records[0].record->unit.output ||
      task.tile != records[0].record->unit.tile)
    return Answer(mismatch());
  using Entry = std::pair<const std::size_t, std::size_t>;
  std::map<std::size_t, std::size_t, std::less<std::size_t>,
           ResourceAllocator<Entry>>
      mapped{std::less<std::size_t>{}, ResourceAllocator<Entry>(root)};
  mapped.emplace(records[0].record->unit.step, task.step);
  ReboundTileCache rebound(root);
  using UnitEntry = std::pair<const TileUnit, std::size_t>;
  std::map<TileUnit, std::size_t, std::less<TileUnit>,
           ResourceAllocator<UnitEntry>>
      units_seen{std::less<TileUnit>{}, ResourceAllocator<UnitEntry>(root)};
  for (std::size_t position = 0; position < records.size(); ++position) {
    const auto& saved = records[position];
    auto status =
        charge(1 + saved.routes.size() + saved.contract.size() + mapped.size());
    if (!status.ok())
      return Answer(status);
    const auto found = mapped.find(saved.record->unit.step);
    if (found == mapped.end() || found->second >= plan.steps().size() ||
        found->second >= contracts.size() ||
        std::string_view(contracts[found->second]) !=
            std::string_view(saved.contract) ||
        saved.routes.size() != saved.record->inputs.size())
      return Answer(mismatch());
    const auto current = found->second;
    const auto& step = plan.steps()[current];
    auto units =
        producer_units(plan, PlanStepInput{current}, saved.record->unit.output);
    if (!units.ok() || saved.record->unit.tile >= units.value().count)
      return Answer(mismatch());
    auto record = std::allocate_shared<TileRecord>(
        ResourceAllocator<TileRecord>(root), root);
    record->unit = {current, saved.record->unit.output,
                    saved.record->unit.tile};
    if (saved.record->computed_descriptor != units.value().computed_descriptor)
      return Answer(mismatch());
    record->computed_descriptor = units.value().computed_descriptor;
    for (std::size_t i = 0; i < saved.routes.size(); ++i) {
      const auto& dependency = saved.record->inputs[i];
      const auto& route = saved.routes[i];
      status = charge(1 + route.ports.size() + route.contract.size() +
                      route.source.size() + dependency.tiles.size());
      if (!status.ok())
        return Answer(status);
      std::optional<PlanInput> target;
      for (auto port : route.ports) {
        if (port >= step.inputs.size())
          return Answer(mismatch());
        const auto& input = step.inputs[port];
        if (input.index() != dependency.producer.index() ||
            (target && !same_input(*target, input)))
          return Answer(mismatch());
        target = input;
      }
      if (!target)
        return Answer(mismatch());
      if (const auto* producer = std::get_if<PlanStepInput>(&*target)) {
        const auto previous =
            std::get<PlanStepInput>(dependency.producer).step_index;
        auto count =
            producer_units(plan, *producer, dependency.producer_output);
        if (!count.ok() || producer->step_index >= contracts.size() ||
            std::string_view(contracts[producer->step_index]) !=
                std::string_view(route.contract) ||
            (!dependency.tiles.empty() &&
             *std::prev(dependency.tiles.end()) >= count.value().count))
          return Answer(mismatch());
        auto inserted = mapped.emplace(previous, producer->step_index);
        if (!inserted.second && inserted.first->second != producer->step_index)
          return Answer(mismatch());
      } else {
        const auto input =
            std::get<PlanWorkflowInput>(*target).declaration_index;
        if (input >= plan.input_declarations().size() ||
            std::string_view(plan.input_declarations()[input].name) !=
                std::string_view(route.source))
          return Answer(mismatch());
        auto grid = TileGrid::from_schema(
            *plan.input_declarations()[input].result_schema,
            plan.tile_extent());
        if (!grid.ok() ||
            (!dependency.tiles.empty() &&
             (!grid.value() || *std::prev(dependency.tiles.end()) >=
                                   grid.value()->tile_count())))
          return Answer(mismatch());
      }
      status = record->include({*target, dependency.producer_output,
                                dependency.whole, dependency.tiles},
                               root);
      if (!status.ok())
        return Answer(status);
    }
    status = charge(units_seen.size() + 1);
    if (!status.ok())
      return Answer(status);
    auto prior = units_seen.find(record->unit);
    if (prior != units_seen.end()) {
      const auto& existing =
          prior->second ? *rebound.ancestry[prior->second - 1] : rebound.task;
      if (record->computed_descriptor != existing.computed_descriptor ||
          record->inputs.size() != existing.inputs.size())
        return Answer(mismatch());
      if (!core_internal::can_multiply(record->inputs.size(),
                                       existing.inputs.size()))
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      status = charge(record->inputs.size() * existing.inputs.size());
      if (!status.ok())
        return Answer(status);
      for (const auto& dependency : record->inputs) {
        const auto found_input = std::find_if(
            existing.inputs.begin(), existing.inputs.end(),
            [&](const auto& entry) {
              return same_input(entry.producer, dependency.producer) &&
                     entry.producer_output == dependency.producer_output;
            });
        if (found_input == existing.inputs.end() ||
            dependency.whole != found_input->whole)
          return Answer(mismatch());
        status = charge(dependency.tiles.size() + found_input->tiles.size());
        if (!status.ok())
          return Answer(status);
        if (dependency.tiles != found_input->tiles)
          return Answer(mismatch());
      }
      continue;
    }
    units_seen.emplace(record->unit,
                       position ? rebound.ancestry.size() + 1 : 0);
    if (!position)
      rebound.task = std::move(*record);
    else
      rebound.ancestry.push_back(std::move(record));
  }
  return Answer(std::move(rebound));
} catch (const std::bad_alloc&) {
  return Result<ReboundTileCache>(Status{ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps::execution_internal
