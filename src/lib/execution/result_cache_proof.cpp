#include "execution/result_cache_proof.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "execution/result_blocks.hpp"

namespace ps::execution_internal {
struct ResultCacheProof::SourceRange {
  enum class Kind { Descriptor, Field, Tensor };
  ResourceString input;
  Kind target;
  std::uint32_t slot;
  Footprint samples;
};
Status ResultCacheProof::hash_facts(content_internal::Sha256& hash,
                                    const ResultRef& object,
                                    const ResultDescriptor& facts) {
  auto charged =
      work_(1 + object.schema().canonical_size() + facts.field_count());
  if (!charged.ok())
    return charged;
  auto schema = object.schema().managed_canonical(root_);
  if (!schema.ok())
    return schema.status();
  hash.text(schema.value());
  hash.integer(facts.field_count());
  for (std::uint32_t field = 0; field < facts.field_count(); ++field)
    hash.integer(facts.rows(field));
  return Status::success();
}
Result<ResourceString> ResultCacheProof::supplied_facts(
    const ResultObjectInputs& results, const ResultTensorInputs& tensors) {
  content_internal::Sha256 hash;
  hash.text("photospider.result-cache-semantic-facts.v1");
  for (const auto& input : results) {
    auto status = work_(1);
    if (!status.ok())
      return Result<ResourceString>(status);
    auto facts = input.second.descriptor(false);
    if (!facts.ok())
      return Result<ResourceString>(facts.status());
    hash.integer(input.first);
    status = hash_facts(hash, input.second, facts.value());
    if (!status.ok())
      return Result<ResourceString>(status);
  }
  for (const auto& input : tensors) {
    auto status = work_(1 + input.second.samples_.boxes().size());
    if (!status.ok())
      return Result<ResourceString>(status);
    hash.integer(input.first.first);
    hash.integer(input.first.second);
    if (input.second.static_schema_) {
      status = work_(input.second.static_schema_->canonical_size() + 1);
      if (status.ok()) {
        hash.integer(0U);
        auto schema = input.second.static_schema_->managed_canonical(root_);
        if (!schema.ok())
          return Result<ResourceString>(schema.status());
        hash.text(schema.value());
      }
    } else {
      hash.integer(1U);
      status = hash_facts(hash, input.second.result_, input.second.descriptor_);
    }
    if (!status.ok())
      return Result<ResourceString>(status);
    append_block_footprint(&hash, input.second.samples_);
  }
  return Result<ResourceString>(
      ResourceString(hash.finish(), ResourceAllocator<char>(root_)));
}
Result<ResourceString> ResultCacheProof::tile_source_digest(
    const ExecutionPlan& plan, const std::vector<ExecutionBinding>& bindings,
    const ResourceVector<TileCacheSource>& sources) {
  // Actual tile Needs select the logical ranges to hash.
  ResourceVector<SourceRange> selected{ResourceAllocator<SourceRange>(root_)};
  FootprintLimits limits;
  limits.cancellation = cancellation_();
  limits.consume_work = work_;
  for (const auto& source : sources) {
    auto status =
        work_(1 + source.name.size() + plan.input_declarations().size() +
              bindings.size() + source.tiles.size());
    if (!status.ok())
      return Result<ResourceString>(status);
    auto declaration = std::find_if(
        plan.input_declarations().begin(), plan.input_declarations().end(),
        [&](const auto& entry) {
          return std::string_view(entry.name) == std::string_view(source.name);
        });
    auto binding =
        std::find_if(bindings.begin(), bindings.end(), [&](const auto& entry) {
          return std::string_view(entry.name) == std::string_view(source.name);
        });
    if (declaration == plan.input_declarations().end() ||
        binding == bindings.end() || !binding->result.valid())
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    const auto& schema = *declaration->result_schema;
    if (!schema.same_schema(binding->result.schema()))
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    auto descriptor = binding->result.descriptor(source.whole);
    if (!descriptor.ok())
      return Result<ResourceString>(descriptor.status());
    auto facts = Footprint::all({1}, limits);
    if (!facts.ok())
      return Result<ResourceString>(facts.status());
    selected.push_back(
        {source.name, SourceRange::Kind::Descriptor, 0, facts.take_value()});
    if (!source.whole && source.tiles.empty())
      continue;
    auto grid = TileGrid::from_schema(schema, plan.tile_extent());
    if (!grid.ok())
      return Result<ResourceString>(grid.status());
    TileRequest request{source.whole, source.tiles};
    auto resolved = request.resolve(grid.value(), grid.value().has_value());
    if (!resolved.ok())
      return Result<ResourceString>(resolved.status());
    for (std::uint32_t slot = 0; slot < schema.fields.size(); ++slot) {
      const auto rows = descriptor.value().rows(slot);
      auto samples =
          rows ? Footprint::all({rows}, limits) : Footprint::none({1}, limits);
      if (!samples.ok())
        return Result<ResourceString>(samples.status());
      selected.push_back(
          {source.name, SourceRange::Kind::Field, slot, samples.take_value()});
    }
    for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
      const auto shape = schema.tensors[slot].sample_shape();
      auto samples = source.whole ? Footprint::all(shape, limits)
                                  : Footprint::none(shape, limits);
      if (!samples.ok())
        return Result<ResourceString>(samples.status());
      for (auto tile : source.tiles) {
        auto region = grid.value()->region(tile);
        if (!region.ok())
          return Result<ResourceString>(region.status());
        auto part =
            Footprint::from_regions(shape, {region.take_value()}, limits);
        if (!part.ok())
          return Result<ResourceString>(part.status());
        samples = samples.value().unite(part.value(), limits);
        if (!samples.ok())
          return Result<ResourceString>(samples.status());
      }
      selected.push_back(
          {source.name, SourceRange::Kind::Tensor, slot, samples.take_value()});
    }
  }
  return source_digest(bindings, selected);
}
Result<ResourceString> ResultCacheProof::source_digest(
    const std::vector<ExecutionBinding>& bindings,
    const ResourceVector<SourceRange>& sources) {
  content_internal::Sha256 hash;
  hash.text("photospider.result-cache-sources.v2");
  auto initial = work_(1 + sources.size());
  if (!initial.ok())
    return Result<ResourceString>(initial);
  for (const auto& source : sources) {
    auto status = work_(bindings.size() + source.input.size() +
                        source.samples.boxes().size());
    if (!status.ok())
      return Result<ResourceString>(status);
    auto found = std::find_if(bindings.begin(), bindings.end(),
                              [&](const auto& binding) {
                                return std::string_view(binding.name) ==
                                       std::string_view(source.input);
                              });
    if (found == bindings.end())
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    hash.text(source.input);
    hash.integer(static_cast<std::uint32_t>(source.target));
    hash.integer(source.slot);
    append_block_footprint(&hash, source.samples);
    if (!found->result.valid())
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    auto descriptor = found->result.descriptor(false);
    if (!descriptor.ok())
      return Result<ResourceString>(descriptor.status());
    if (source.target == SourceRange::Kind::Descriptor) {
      auto charged = work_(found->result.schema().canonical_size() + 1 +
                           descriptor.value().field_count() +
                           descriptor.value().tensor_count());
      if (!charged.ok())
        return Result<ResourceString>(charged);
      charged = hash_facts(hash, found->result, descriptor.value());
      if (!charged.ok())
        return Result<ResourceString>(charged);
      continue;
    }
    auto count = source.samples.element_count();
    if (!count.ok())
      return Result<ResourceString>(count.status());
    if (source.target == SourceRange::Kind::Field) {
      auto width = found->result.schema().row_bytes(source.slot);
      if (!width.ok() || width.value() > maximum_window_ ||
          !core_internal::can_multiply(count.value(), width.value()))
        return Result<ResourceString>(Status{ErrorCode::ResourceExhausted, {}});
      status = work_(count.value() * width.value());
      if (!status.ok())
        return Result<ResourceString>(status);
      status = source.samples.visit(
          [&](const auto& at) {
            auto active = work_(0);
            if (!active.ok())
              return active;
            auto plan = found->result.prepare_read(descriptor.value(),
                                                   source.slot, at[0], 1);
            if (!plan.ok())
              return plan.status();
            auto bytes = plan.value().load(maximum_window_, cancellation_());
            if (!bytes.ok())
              return bytes.status();
            hash.bytes(bytes.value()->bytes().data(),
                       bytes.value()->bytes().size());
            return Status::success();
          },
          count.value(), cancellation_());
    } else if (source.target == SourceRange::Kind::Tensor) {
      if (source.slot >= found->result.schema().tensors.size())
        return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
      const auto width = Value::element_size(
          found->result.schema().tensors[source.slot].descriptor.element_type);
      status = work_(count.value());
      if (!status.ok())
        return Result<ResourceString>(status);
      for (const auto& box : source.samples.boxes()) {
        auto window = found->result.acquire_tensor(
            descriptor.value(), source.slot, box, cancellation_());
        if (!window.ok())
          return Result<ResourceString>(window.status());
        auto samples = Footprint::from_regions(source.samples.shape(), {box});
        if (!samples.ok())
          return Result<ResourceString>(samples.status());
        status = samples.value().visit(
            [&](const auto& at) {
              auto active = work_(0);
              if (!active.ok())
                return active;
              auto run = window.value().row_run(at);
              if (!run.ok())
                return run.status();
              hash.bytes(run.value().data, width);
              return Status::success();
            },
            count.value(), cancellation_());
        if (!status.ok())
          break;
      }
    } else {
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    }
    if (!status.ok())
      return Result<ResourceString>(status);
  }
  auto active = work_(0);
  if (!active.ok())
    return Result<ResourceString>(active);
  return Result<ResourceString>(
      ResourceString(hash.finish(), ResourceAllocator<char>(root_)));
}
}  // namespace ps::execution_internal
