#include "execution/structured_result_cache.hpp"

#include <algorithm>
#include <functional>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "execution/result_blocks.hpp"
#include "execution/result_cache_proof.hpp"
#include "photospider/core/checked_math.hpp"

namespace ps::execution_internal {
Status StructuredResultCache::charge(std::uint64_t units,
                                     std::recursive_mutex& gate,
                                     StructuredCacheWorkServices& host,
                                     std::uint64_t& observed) {
  std::lock_guard<std::recursive_mutex> lock(gate);
  const auto stopped = host.stop();
  if (stopped != ErrorCode::Ok)
    return Status{stopped, {}};
  auto status = quota_.charge(units);
  if (status.ok())
    observed += units;
  return status;
}
Result<ResourceString> StructuredResultCache::supplied_facts(
    const ResultObjectInputs& results, const ResultTensorInputs& tensors,
    std::uint64_t maximum_window, StructuredCacheWorkServices& host) {
  const std::function<Status(std::uint64_t)> work = [&](auto n) {
    return host.charge(n);
  };
  const std::function<CancellationToken()> cancellation = [&] {
    return host.cancellation();
  };
  return ResultCacheProof(resources_, maximum_window, work, cancellation)
      .supplied_facts(results, tensors);
}
Result<ResourceString> StructuredResultCache::source_digest(
    const StructuredCacheContext& ctx,
    const ResourceVector<TileCacheSource>& sources,
    StructuredCacheWorkServices& host) {
  const std::function<Status(std::uint64_t)> work = [&](auto n) {
    return host.charge(n);
  };
  const std::function<CancellationToken()> cancellation = [&] {
    return host.cancellation();
  };
  return ResultCacheProof(resources_, ctx.maximum_window, work, cancellation)
      .tile_source_digest(ctx.plan, ctx.bindings, sources);
}

std::string StructuredResultCache::completed_key(
    std::size_t index, const ResultProgramQuery& query,
    const StructuredCacheContext& ctx) {
  content_internal::Sha256 hash;
  hash.text("photospider.completed-tile.v1");
  hash.text(ctx.templates[index]);
  hash.integer(query.output_index);
  hash.integer(static_cast<std::uint32_t>(ctx.plan.steps()[index].backend));
  hash.integer(query.tile_extent);
  hash.integer(query.tensor_slot);
  hash.integer(query.tile_unit);
  return hash.finish();
}
void StructuredResultCache::record_need(
    std::size_t index, StructuredCacheActorView actor,
    const StructuredCacheContext& ctx, const ResultProgramNeed& need,
    StructuredCacheWorkServices& host) noexcept {
  if (!ctx.table || !ctx.eligible[index] || actor.cache.disabled)
    return;
  try {
    auto count = 1 + need.tiles.size();
    if (actor.cache.replay.size() >= ctx.table->dependency_metadata_limit() ||
        !host.charge(count).ok()) {
      actor.cache.disabled = true;
      actor.cache.replay.clear();
      return;
    }
    auto facts =
        supplied_facts(actor.results, actor.tensors, ctx.maximum_window, host);
    if (!facts.ok()) {
      actor.cache.disabled = true;
      actor.cache.replay.clear();
      return;
    }
    StructuredCacheNeed saved;
    saved.request.tiles = need.tiles;
    saved.facts = facts.take_value();
    actor.cache.replay.push_back(std::move(saved));
  } catch (...) {
    actor.cache.disabled = true;
    actor.cache.replay.clear();
  }
}
void StructuredResultCache::store_completed(
    std::size_t index, StructuredCacheActorView actor,
    const StructuredCacheContext& ctx,
    StructuredCacheWorkServices& host) noexcept {
  if (!ctx.table || !ctx.eligible[index] || actor.cache.disabled ||
      actor.fallback_taint ||
      actor.query.backend != ctx.plan.steps()[index].backend || !remaining() ||
      ctx.table->epoch() != ctx.epoch)
    return;
  try {
    auto tiles = TileCacheSnapshot::capture(
        resources_, ctx.plan, ctx.templates, ctx.records, actor.tile_record,
        ctx.table->dependency_metadata_limit(),
        [&](std::uint64_t n) { return host.charge(n); });
    if (!tiles.ok())
      return;
    auto content = source_digest(ctx, tiles.value()->sources, host);
    if (!content.ok())
      return;
    auto charged = host.charge(1 + actor.cache.replay.size() +
                               tiles.value()->sources.size());
    if (!charged.ok())
      return;
    auto manifest = std::allocate_shared<StructuredCacheManifest>(
        ResourceAllocator<StructuredCacheManifest>(resources_));
    manifest->content = content.take_value();
    manifest->tiles = tiles.take_value();
    manifest->replay = actor.cache.replay;
    manifest->result = actor.published;
    manifest->backend = actor.query.backend;
    manifest->epoch = ctx.epoch;
    manifest->metadata =
        1 + manifest->replay.size() + manifest->tiles->metadata;
    for (const auto& log : manifest->replay) {
      manifest->metadata += log.facts.size() + log.request.tiles.size();
      for (const auto& tensor : log.request.tiles)
        manifest->metadata += tensor.tiles.size() + 1;
    }
    if (manifest->metadata > ctx.table->dependency_metadata_limit() ||
        !host.charge(manifest->metadata).ok())
      return;
    const auto key = completed_key(index, actor.query, ctx);
    content_internal::Sha256 storage;
    storage.text("photospider.completed-result-payload.v1");
    storage.text(key);
    storage.text(manifest->content);
    for (const auto& log : manifest->replay)
      storage.text(log.facts);
    manifest->key = ResourceString("result/" + storage.finish(),
                                   ResourceAllocator<char>(resources_));
    ctx.table->put_structured(key, std::move(manifest),
                              [&](std::uint64_t n) { return host.charge(n); });
  } catch (...) {
  }
}
Result<bool> StructuredResultCache::try_reuse(std::size_t index,
                                              StructuredCacheActorView actor,
                                              const StructuredCacheContext& ctx,
                                              StructuredCacheReplayHost& host) {
  if (!ctx.table || !ctx.plan.steps()[index].output_result_schema ||
      !ctx.eligible[index] || !remaining())
    return Result<bool>(false);
  bool replayed = false;
  struct InitialState {
    ResultTensorInputs tensors;
    ResultObjectInputs results;
    ResultDescriptorInputs descriptors;
    TileRecord tile_record;
  };
  std::optional<InitialState> initial;
  const auto reset = [&]() {
    actor.busy = false;
    actor.cache.replay.clear();
    actor.tensors.clear();
    actor.results.clear();
    actor.descriptors.clear();
    actor.tile_record.inputs.clear();
    if (initial) {
      actor.tensors = std::move(initial->tensors);
      actor.results = std::move(initial->results);
      actor.descriptors = std::move(initial->descriptors);
      actor.tile_record = std::move(initial->tile_record);
    }
    return Status::success();
  };
  try {
    auto charged = host.charge(ctx.templates[index].size() + 1);
    if (!charged.ok())
      return charged.code == ErrorCode::ResourceExhausted
                 ? Result<bool>(false)
                 : Result<bool>(charged);
    auto candidates = ctx.table->structured_candidates(
        completed_key(index, actor.query, ctx), ctx.epoch);
    if (candidates.empty())
      return Result<bool>(false);
    for (const auto& candidate : candidates) {
      if (!candidate->tiles)
        continue;
      auto rebound = candidate->tiles->rebind(
          resources_, ctx.plan, ctx.templates, actor.tile_record.unit,
          [&](std::uint64_t n) { return host.charge(n); });
      if (!rebound.ok()) {
        if (rebound.status().code == ErrorCode::Cancelled ||
            rebound.status().code == ErrorCode::Stale)
          return Result<bool>(rebound.status());
        continue;
      }
      auto content = source_digest(ctx, candidate->tiles->sources, host);
      if (!content.ok()) {
        if (content.status().code == ErrorCode::Cancelled ||
            content.status().code == ErrorCode::Stale)
          return Result<bool>(content.status());
        continue;
      }
      if (content.value() != candidate->content)
        continue;
      charged = host.charge(1 + 64 * actor.tensors.size() +
                            actor.results.size() + actor.descriptors.size());
      if (!charged.ok())
        return charged.code == ErrorCode::ResourceExhausted
                   ? Result<bool>(false)
                   : Result<bool>(charged);
      initial.emplace(InitialState{actor.tensors, actor.results,
                                   actor.descriptors, actor.tile_record});
      bool matched = true;
      replayed = true;
      actor.busy = true;
      for (const auto& log : candidate->replay) {
        auto status = host.supply(log.request);
        if (!status.ok()) {
          actor.busy = false;
          if (status.code == ErrorCode::Cancelled ||
              status.code == ErrorCode::Stale)
            return Result<bool>(status);
          matched = false;
          break;
        }
        auto facts = supplied_facts(actor.results, actor.tensors,
                                    ctx.maximum_window, host);
        if (!facts.ok() || facts.value() != log.facts) {
          matched = false;
          break;
        }
      }
      actor.busy = false;
      if (actor.fallback_taint) {
        actor.cache.disabled = true;
        matched = false;
      }
      if (!matched) {
        break;
      }
      if (!ctx.table->structured_verified(candidate))
        break;
      auto adopted = host.adopt(*candidate.operator->(), rebound.value());
      if (!adopted.ok())
        return adopted;
      if (adopted.value())
        return adopted;
      break;
    }
    if (!replayed)
      return Result<bool>(false);
    auto scalar = reset();
    return scalar.ok() ? Result<bool>(false) : Result<bool>(scalar);
  } catch (const std::bad_alloc&) {
    if (!replayed)
      return Result<bool>(false);
    try {
      auto status = reset();
      return status.ok() ? Result<bool>(false) : Result<bool>(status);
    } catch (const std::bad_alloc&) {
      return Result<bool>(Status{ErrorCode::ResourceExhausted, {}});
    }
  }
}
}  // namespace ps::execution_internal
