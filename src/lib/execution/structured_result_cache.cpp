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
    const ResourceVector<SourceObservation>& sources,
    StructuredCacheWorkServices& host) {
  const std::function<Status(std::uint64_t)> work = [&](auto n) {
    return host.charge(n);
  };
  const std::function<CancellationToken()> cancellation = [&] {
    return host.cancellation();
  };
  return ResultCacheProof(resources_, ctx.maximum_window, work, cancellation)
      .source_digest(ctx.bindings, sources);
}

std::string StructuredResultCache::completed_key(
    std::size_t index, const ResultProgramQuery& query,
    const StructuredCacheContext& ctx) {
  content_internal::Sha256 hash;
  hash.text("photospider.completed-result.v1");
  hash.text(ctx.templates[index]);
  hash.integer(query.output_index);
  hash.integer(static_cast<std::uint32_t>(ctx.plan.steps()[index].backend));
  hash.integer(query.tensor_slot);
  hash.integer(query.tensor_outputs.has_value());
  if (query.tensor_outputs)
    append_block_footprint(&hash, *query.tensor_outputs);
  return hash.finish();
}
void StructuredResultCache::record_need(
    std::size_t index, StructuredCacheActorView actor,
    const StructuredCacheContext& ctx, const ResultProgramNeed& need,
    StructuredCacheWorkServices& host) noexcept {
  if (!ctx.table || !ctx.eligible[index] || actor.cache.disabled)
    return;
  try {
    auto count = 1 + need.tensors.size() + need.results.size();
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
    saved.request.tensors = need.tensors;
    saved.request.results = need.results;
    saved.facts = facts.take_value();
    actor.cache.replay.push_back(std::move(saved));
  } catch (...) {
    actor.cache.disabled = true;
    actor.cache.replay.clear();
  }
}
Result<ResourceVector<SourceObservation>> StructuredResultCache::source_proof(
    std::size_t index, const ResultRef& result,
    const StructuredCacheContext& ctx, StructuredCacheWorkServices& host) {
  auto bundle = result.dependencies();
  if (!bundle)
    return Result<ResourceVector<SourceObservation>>(
        Status{ErrorCode::NotFound, {}});
  auto limits = ctx.limits;
  limits.maximum_work = std::min(limits.maximum_work, remaining());
  limits.consume_work = [&](std::uint64_t n) { return host.charge(n); };
  limits.cancellation = host.cancellation();
  auto charged = host.charge(1 + ctx.plan.input_declarations().size() +
                             ctx.snapshot.size() + ctx.bindings.size());
  if (!charged.ok())
    return Result<ResourceVector<SourceObservation>>(charged);
  for (const auto& declaration : ctx.plan.input_declarations()) {
    const auto rank =
        declaration.result_schema && !declaration.result_schema->tensors.empty()
            ? declaration.result_schema->tensors[0].batch_axes.size() +
                  declaration.result_schema->tensors[0].descriptor.shape.size()
            : 0;
    charged = host.charge(1 + declaration.name.size() + rank);
    if (!charged.ok())
      return Result<ResourceVector<SourceObservation>>(charged);
  }
  DependencyRecords proof(ctx.plan, std::string(ctx.snapshot), limits);
  for (std::size_t i = 0; i < ctx.bindings.size(); ++i)
    if (ctx.bindings[i].result.valid()) {
      const auto& schema = ctx.bindings[i].result.schema();
      charged = host.charge(1 + schema.fields.size() + schema.tensors.size());
      if (!charged.ok())
        return Result<ResourceVector<SourceObservation>>(charged);
      for (const auto& tensor : schema.tensors) {
        charged = host.charge(2 + tensor.descriptor.shape.size() +
                              tensor.batch_axes.size());
        if (!charged.ok())
          return Result<ResourceVector<SourceObservation>>(charged);
      }
      auto status =
          proof.bind_result(PlanWorkflowInput{i}, ctx.bindings[i].result);
      if (!status.ok())
        return Result<ResourceVector<SourceObservation>>(status);
    }
  auto imported = proof.import_bundle(
      *bundle, index, [&](std::uint64_t n) { return host.charge(n); });
  if (!imported.ok())
    return Result<ResourceVector<SourceObservation>>(imported);
  return proof.source_observations();
}
void StructuredResultCache::store_completed(
    std::size_t index, StructuredCacheActorView actor,
    const StructuredCacheContext& ctx,
    StructuredCacheWorkServices& host) noexcept {
  if (!ctx.table || !ctx.eligible[index] || actor.cache.disabled ||
      actor.fallback_taint || actor.quality ||
      actor.query.backend != ctx.plan.steps()[index].backend || !remaining() ||
      ctx.table->epoch() != ctx.epoch)
    return;
  try {
    auto sources = source_proof(index, actor.published, ctx, host);
    if (!sources.ok())
      return;
    auto content = source_digest(ctx, sources.value(), host);
    if (!content.ok())
      return;
    auto charged =
        host.charge(1 + actor.cache.replay.size() + sources.value().size());
    if (!charged.ok())
      return;
    auto manifest = std::allocate_shared<StructuredCacheManifest>(
        ResourceAllocator<StructuredCacheManifest>(resources_));
    manifest->content = content.take_value();
    manifest->sources = sources.take_value();
    manifest->replay = actor.cache.replay;
    manifest->obligations = actor.input_obligations;
    manifest->bundle = actor.published.dependencies();
    manifest->result = actor.published;
    manifest->backend = actor.query.backend;
    manifest->epoch = ctx.epoch;
    manifest->metadata = 1 + manifest->replay.size() + manifest->sources.size();
    const auto maximum = ctx.table->dependency_metadata_limit();
    const auto add_metadata = [&](std::uint64_t count) {
      if (count > maximum || manifest->metadata > maximum - count)
        return false;
      manifest->metadata += count;
      return host.charge(count).ok();
    };
    ResourceVector<const void*> relation_owners;
    ResourceVector<const DependencyRecord*> pending;
    std::set<const DependencyRecord*, std::less<const DependencyRecord*>,
             ResourceAllocator<const DependencyRecord*>>
        seen;
    if (!add_metadata(manifest->bundle->roots.size()))
      return;
    std::set<const TerminalResultRequest*,
             std::less<const TerminalResultRequest*>,
             ResourceAllocator<const TerminalResultRequest*>>
        requests;
    for (const auto& record : manifest->bundle->roots)
      pending.push_back(record.get());
    while (!pending.empty()) {
      if (!host.charge(1).ok())
        return;
      const auto* record = pending.back();
      pending.pop_back();
      if (!seen.insert(record).second)
        continue;
      ++ctx.visited_records;
      if (record->request && requests.insert(record->request.get()).second) {
        if (!add_metadata(1 + record->request->identity.size() +
                          record->request->manifest.size()))
          return;
        for (const auto& need : record->request->manifest)
          if (!add_metadata(need.tags.size() * 3 + need.samples.shape().size() +
                            need.samples.boxes().size() *
                                (1 + 2 * need.samples.shape().size())))
            return;
      }

      if (!add_metadata(1 + record->identity.size() + record->scope.size() +
                        record->input_queries.size() +
                        record->samples.shape().size() +
                        record->samples.boxes().size() *
                            (1 + 2 * record->samples.shape().size()) +
                        record->routes.size() + record->upstream.size() +
                        record->upstream_ports.size() + record->domains.size() +
                        record->manifest.size()))
        return;
      for (const auto& input : record->input_queries)
        if (!add_metadata(input.identity.size()))
          return;
      for (const auto& domain : record->domains)
        if (!add_metadata(domain.shape.size()))
          return;
      for (const auto& need : record->manifest)
        if (!add_metadata(need.tags.size() * 3 + need.samples.shape().size() +
                          need.samples.boxes().size() *
                              (1 + 2 * need.samples.shape().size())))
          return;
      if (record->certificate &&
          !add_metadata(record->certificate->storage_entries()))
        return;
      for (const auto& relation : {record->relation, record->descriptor}) {
        auto weight = relation.cache_metadata(
            relation_owners, maximum - manifest->metadata,
            [&](std::uint64_t n) { return host.charge(n); });
        if (!weight.ok())
          return;
        manifest->metadata += weight.value();
      }
      for (const auto& child : record->upstream)
        pending.push_back(child.get());
    }
    for (const auto& source : manifest->sources)
      if (!add_metadata(source.input.size() + source.samples.shape().size() +
                        source.samples.boxes().size() *
                            (1 + 2 * source.samples.shape().size())))
        return;
    auto obligations = manifest->obligations.cache_metadata(
        relation_owners, maximum - manifest->metadata,
        [&](std::uint64_t n) { return host.charge(n); });
    if (!obligations.ok())
      return;
    manifest->metadata += obligations.value();
    for (const auto& log : manifest->replay) {
      manifest->metadata += log.facts.size() + log.request.results.size() +
                            log.request.tensors.size();
      for (const auto& tensor : log.request.tensors)
        manifest->metadata +=
            tensor.samples.boxes().size() + tensor.samples.shape().size();
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
      !ctx.eligible[index] || !remaining() ||
      actor.query.output.result_schema->id == "photospider.path_set")
    return Result<bool>(false);
  bool replayed = false;
  struct InitialState {
    ResultTensorInputs tensors;
    ResultObjectInputs results;
    ResultNeedHistory history;
    ResultInputFacts facts;
    ResultRelation obligations;
    ResourceVector<StructuredInputBundle> bundles;
  };
  std::optional<InitialState> initial;
  const auto reset = [&]() {
    actor.busy = false;
    actor.cache.replay.clear();
    actor.io.clear();
    actor.tensors.clear();
    actor.results.clear();
    actor.history.clear();
    actor.input_facts.clear();
    actor.input_bundles.clear();
    actor.input_obligations = {};
    if (initial) {
      actor.tensors = std::move(initial->tensors);
      actor.results = std::move(initial->results);
      actor.history = std::move(initial->history);
      actor.input_facts = std::move(initial->facts);
      actor.input_obligations = std::move(initial->obligations);
      actor.input_bundles = std::move(initial->bundles);
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
      auto content = source_digest(ctx, candidate->sources, host);
      if (!content.ok()) {
        if (content.status().code == ErrorCode::Cancelled ||
            content.status().code == ErrorCode::Stale)
          return Result<bool>(content.status());
        continue;
      }
      if (content.value() != candidate->content)
        continue;
      charged =
          host.charge(1 + 64 * actor.tensors.size() + actor.results.size() +
                      actor.history.size() + actor.input_facts.size());
      if (!charged.ok())
        return charged.code == ErrorCode::ResourceExhausted
                   ? Result<bool>(false)
                   : Result<bool>(charged);
      initial.emplace(InitialState{actor.tensors, actor.results, actor.history,
                                   actor.input_facts, actor.input_obligations,
                                   actor.input_bundles});
      bool matched = true;
      replayed = true;
      actor.busy = true;
      for (const auto& log : candidate->replay) {
        auto status = host.supply(log.request);
        if (!status.ok()) {
          actor.busy = false;
          if (host.detaching())
            reset();
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
      ResourceVector<std::uint64_t> association{
          ResourceAllocator<std::uint64_t>(resources_)};
      for (const auto& facts : actor.input_facts)
        association.push_back(facts.first.second);
      auto adopted = host.adopt(*candidate.operator->(), association);
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
