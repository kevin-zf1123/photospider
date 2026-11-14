#include "sdk/operation/tile_mapping.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "photospider/core/callback_environment.hpp"
#include "photospider/core/content_digest.hpp"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps {
LoweringSession::LoweringSession(const physical::ValidationLimits& limits)
    : budget_([&] {
        ResourceLimits resources;
        resources.maximum_work = limits.maximum_work;
        return resources;
      }()) {}
Result<std::vector<TileNeed>> LoweringSession::solve(
    const MapperDefinition& m, const MappingQuery& q,
    const physical::ValidationLimits& limits) {
  using Answer = Result<std::vector<TileNeed>>;
  content_internal::Sha256 key;
  auto schema = [&](const OperationMetadata& meta) {
    key.integer(static_cast<bool>(meta.result_schema));
    if (meta.result_schema) {
      auto bytes = meta.result_schema->managed_canonical(budget_);
      if (!bytes.ok())
        throw bytes.status();
      auto cost = budget_.consume({1 + bytes.value().size() / 64});
      if (!cost.ok())
        throw cost;
      key.text(std::string_view(bytes.value().data(), bytes.value().size()));
    }
  };
  try {
    key.text(m.contract);
    key.text(m.provider);
    key.integer(m.version);
    key.integer(m.revision);
    key.integer(limits.maximum_records);
    key.integer(limits.maximum_edges);
    key.integer(q.output);
    key.integer(q.member);
    key.integer(q.tile);
    key.integer(q.tile_extent);
    key.integer(q.region.rank());
    for (const auto& d : q.region.dimensions()) {
      key.integer(d.offset);
      key.integer(d.extent);
    }
    schema(q.output_metadata);
    key.integer(q.sources.size());
    for (const auto& s : q.sources) {
      schema(s.metadata);
      key.integer(s.input);
      key.integer(s.member);
      key.integer(s.grid.rank());
      key.integer(s.grid.tile_extent());
      key.integer(s.grid.batch_axis_mask());
      key.integer(s.grid.height_axis());
      key.integer(s.grid.width_axis());
      for (auto n : s.grid.shape())
        key.integer(n);
    }
    key.integer(q.config.size());
    for (auto word : q.config)
      key.integer(word);
    auto identity = key.finish();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (previous_ && previous_->key == identity) {
        auto cost = budget_.consume({1 + previous_->needs.size()});
        if (!cost.ok())
          return Answer(cost);
        return Answer(previous_->needs);
      }
    }
    auto result = plugin_internal::solve_mapping(q, m.solve, *this, limits);
    if (result.ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      previous_ = Cached{std::move(identity), result.value()};
    }
    return result;
  } catch (const Status& error) {
    return Answer(error);
  }
}
MappingSink::MappingSink(const MappingQuery& query,
                         const ResourceBudget& budget,
                         const physical::ValidationLimits& limits)
    : query_(query), budget_(budget), limits_(limits) {}
Status MappingSink::remember(Status status) {
  if (status_.ok())
    status_ = std::move(status);
  return status_;
}
Status MappingSink::consume(std::uint64_t work) {
  return status_.ok() ? remember(budget_.consume({work})) : status_;
}
Status MappingSink::tiles(std::uint32_t slot, TileSet set) {
  if (!status_.ok())
    return status_;
  if (slot >= query_.sources.size())
    return remember({ErrorCode::InvalidArgument, "invalid mapper source slot"});
  const auto& source = query_.sources[slot];
  if (!set.empty() && set.intervals().back().end > source.grid.tile_count())
    return remember({ErrorCode::InvalidArgument, "mapper tile outside grid"});
  if (set.empty())
    return status_;
  if (needs_.size() >= limits_.maximum_records ||
      set.size() > limits_.maximum_edges - edges_)
    return remember({ErrorCode::ResourceExhausted, "mapper read limit"});
  if (consume(set.size()).ok()) {
    edges_ += set.size();
    needs_.push_back({source.input, source.member, false, std::move(set)});
  }
  return status_;
}
Status MappingSink::region(std::uint32_t slot, const Region& region) {
  if (!status_.ok())
    return status_;
  if (slot >= query_.sources.size())
    return remember({ErrorCode::InvalidArgument, "invalid mapper source slot"});
  auto set = sdk::tiles_for_region(budget_, query_.sources[slot].grid, region);
  return set.ok() ? tiles(slot, set.take_value()) : remember(set.status());
}
Status MappingSink::interval(std::uint32_t slot, TileIndex begin,
                             TileIndex end) {
  if (!status_.ok())
    return status_;
  auto set = TileSet::range(budget_, begin, end);
  return set.ok() ? tiles(slot, set.take_value()) : remember(set.status());
}
Status MappingSink::regions(std::uint32_t slot,
                            const std::vector<Region>& regions) {
  if (!status_.ok())
    return status_;
  if (slot >= query_.sources.size())
    return remember({ErrorCode::InvalidArgument, "invalid mapper source slot"});
  TileSet merged;
  for (const auto& r : regions) {
    auto part = sdk::tiles_for_region(budget_, query_.sources[slot].grid, r);
    if (!part.ok())
      return remember(part.status());
    auto joined = merged.unite(part.value(), budget_);
    if (!joined.ok())
      return remember(joined.status());
    merged = joined.take_value();
  }
  return tiles(slot, std::move(merged));
}
Status MappingSink::descriptor(std::uint32_t slot) {
  if (!status_.ok())
    return status_;
  if (slot >= query_.sources.size())
    return remember({ErrorCode::InvalidArgument, "invalid mapper schema slot"});
  if (needs_.size() >= limits_.maximum_records)
    return remember({ErrorCode::ResourceExhausted, "mapper descriptor limit"});
  if (consume(1).ok())
    needs_.push_back({query_.sources[slot].input, 0, true, {}});
  return status_;
}
namespace plugin_internal {
Result<std::vector<TileNeed>> solve_mapping(
    const MappingQuery& q, const MappingSolver& solve,
    const LoweringSession& session, const physical::ValidationLimits& limits) {
  using Answer = Result<std::vector<TileNeed>>;
  MappingSink sink(q, session.budget(), limits);
  auto status = sink.consume(1);
  if (status.ok())
    status = core_internal::invoke_callback([&] { return solve(q, sink); });
  if (!sink.status().ok())
    return Answer(sink.status());
  return status.ok() ? Answer(sink.take()) : Answer(status);
}
Result<OperationProgram> lower_tilewise(const OperationLowering& q,
                                        const TilewiseLowering& d,
                                        const OperationTraits& traits) {
  using Answer = Result<OperationProgram>;
  OperationProgramBuilder builder(q);
  MappingQuery map;
  map.tile_extent = q.tile_extent;
  if (d.configuration) {
    auto config = d.configuration(q);
    if (!config.ok())
      return Answer(config.status());
    map.config = config.take_value();
    auto admitted = q.session->budget().consume({map.config.size()});
    if (!admitted.ok())
      return Answer(admitted);
  }
  for (std::uint32_t output = 0; output < q.requests.size(); ++output) {
    const auto& projection = traits.outputs[output].input_indices;
    for (const auto& member : q.requests[output].members) {
      const auto m = member.first;
      map.output = output;
      map.member = m;
      map.output_metadata = q.outputs[output];
      auto grid = TileGrid::from_schema(*map.output_metadata.result_schema,
                                        q.tile_extent, m);
      if (!grid.ok())
        return Answer(grid.status());
      map.sources.clear();
      const std::size_t count =
          projection ? projection->size() : q.inputs.size();
      for (std::size_t i = 0; i < count; ++i) {
        auto port =
            static_cast<std::uint32_t>(projection ? projection->at(i) : i);
        if (port >= q.inputs.size())
          return Answer(
              {ErrorCode::InvalidArgument, "invalid mapper projection"});
        const auto& schema = *q.inputs[port].result_schema;
        // Explicit solvers can select any member; Direct has a fixed
        // correspondence.
        for (std::uint32_t input_member = 0;
             input_member < schema.tensors.size(); ++input_member) {
          if (!d.local && d.external.empty() && input_member != m)
            continue;
          auto source =
              TileGrid::from_schema(schema, q.tile_extent, input_member);
          if (!source.ok())
            return Answer(source.status());
          if (!d.local && d.external.empty() &&
              (m >= schema.tensors.size() ||
               source.value().rank() != grid.value().rank() ||
               source.value().shape() != grid.value().shape()))
            return Answer({ErrorCode::TypeMismatch,
                           "direct mapping requires matching logical shape"});
          map.sources.push_back(
              {port, input_member, source.take_value(), q.inputs[port]});
        }
        if (!d.local && d.external.empty() && m >= schema.tensors.size())
          return Answer({ErrorCode::TypeMismatch,
                         "direct mapping requires corresponding member"});
      }
      for (auto tile : member.second) {
        map.tile = tile;
        auto region = grid.value().region(tile);
        if (!region.ok())
          return Answer(region.status());
        map.region = region.take_value();
        auto needs =
            d.external.empty()
                ? solve_mapping(
                      map,
                      d.local ? d.local
                              : MappingSolver([](const MappingQuery& query,
                                                 MappingSink& sink) {
                                  for (std::uint32_t i = 0;
                                       i < query.sources.size(); ++i) {
                                    auto status = sink.region(i, query.region);
                                    if (!status.ok())
                                      return status;
                                  }
                                  return Status::success();
                                }),
                      *q.session, q.limits)
                : q.map(d.external, map);
        if (!needs.ok())
          return Answer(needs.status());
        auto description = d.describe(q, map);
        if (!description.ok())
          return Answer(description.status());
        auto task = description.take_value();
        auto admitted = q.session->budget().consume({task.parameters.size()});
        if (!admitted.ok())
          return Answer(admitted);
        auto id = builder.add_task(task.symbol, task.contract, task.placement,
                                   task.scratch, false, task.invocation_abi);
        if (!id.ok())
          return Answer(id.status());
        builder.parameters(id.value()) = std::move(task.parameters);
        for (const auto& need : needs.value()) {
          if (need.descriptor_only) {
            auto status = builder.read_input_schema(id.value(), need.input);
            if (!status.ok())
              return Answer(status);
          } else {
            for (auto index : need.tiles) {
              auto status =
                  builder.read_input(id.value(), need.input, need.tensor_slot,
                                     index, task.placement.domain);
              if (!status.ok())
                return Answer(status);
            }
          }
        }
        auto status = builder.write_output(id.value(), output, m, tile);
        if (!status.ok())
          return Answer(status);
      }
    }
  }
  return builder.finish();
}
}  // namespace plugin_internal
}  // namespace ps
