#include "plugins/executors/physical/tile_cache.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "photospider/core/content_digest.hpp"

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace ps::execution_internal {
namespace {
using Id = physical::Id;
using Hash = content_internal::Sha256;
void layout_key(Hash* hash, const physical::Layout& layout) {
  hash->integer(static_cast<unsigned>(layout.element_type));
  hash->integer(layout.rank);
  hash->integer(layout.byte_offset);
  for (unsigned i = 0; i < layout.rank; ++i) {
    hash->integer(layout.origin[i]);
    hash->integer(layout.shape[i]);
    hash->integer(layout.strides[i]);
  }
}
void view_key(Hash* hash, const physical::Program& p, Id tile, Id port,
              Id layout) {
  const auto& t = p.tiles[tile];
  hash->integer(port);
  hash->integer(t.member);
  hash->integer(t.tile_index);
  hash->integer(t.bytes);
  hash->integer(t.alignment);
  hash->integer(static_cast<unsigned>(t.domain));
  layout_key(hash, p.layouts[layout]);
}
TileKey call_key(const ExecutionPlan& plan, Id id) {
  const auto& call = plan.calls()[id];
  if (!call.prepared || !call.prepared->reusable_tiles())
    return {};
  const auto& p = plan.program();
  Hash h;
  h.integer(p.calls[id].source_call);
  h.text(call.operation);
  h.integer(p.tile_extent);
  h.integer(call.parameters.size());
  for (const auto& kv : call.parameters) {
    h.text(kv.first);
    h.integer(kv.second.index());
    if (auto v = std::get_if<std::string>(&kv.second))
      h.text(*v);
    else if (auto v = std::get_if<double>(&kv.second))
      h.bytes(v, sizeof(*v));
    else if (auto v = std::get_if<bool>(&kv.second))
      h.integer(*v);
    else
      h.integer(std::get<std::int64_t>(kv.second));
  }
  for (const auto* group : {&call.inputs, &call.outputs}) {
    h.integer(group->size());
    for (auto value : *group) {
      const auto bytes = p.schemas[p.values[value].schema]->canonical();
      h.text({bytes.data(), bytes.size()});
    }
  }
  for (const auto& r : call.prepared->resource_ids()) {
    h.integer(r.byte_length);
    h.bytes(r.sha256.data(), r.sha256.size());
  }
  return h.digest();
}
}  // namespace

TileCache::TileCache(const ResourceBudget& root, std::uint64_t limit)
    : limit_(limit),
      recent_(ResourceAllocator<Entry>(root)),
      index_(typename decltype(index_)::allocator_type(root)),
      inputs_(typename decltype(inputs_)::allocator_type(root)) {
}  // NOLINT(whitespace/indent_namespace)

TileKey TileCache::input_key(const Owner& owner, std::uint64_t offset,
                             std::uint64_t bytes, const CheckReuse& check) {
  const Slice slice{reinterpret_cast<std::uintptr_t>(owner.get()), offset,
                    bytes};
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = inputs_.find(slice);
    if (it != inputs_.end() && it->second.owner.lock() == owner)
      return it->second.digest;
  }
  const auto backing = owner->bytes();
  if (offset > backing.size() || bytes > backing.size() - offset)
    return {};
  const auto* data = backing.data() + offset;
  // Use the system SHA implementation on Apple Silicon. Hash in bounded
  // spans so a large single tile cannot mask cancellation.
#if defined(__APPLE__)
  CC_SHA256_CTX hash;
  CC_SHA256_Init(&hash);
#else
  Hash hash;
#endif
  for (std::uint64_t pos = 0; pos < bytes;) {
    check();
    const auto n = std::min<std::uint64_t>(bytes - pos, 64U << 10);
#if defined(__APPLE__)
    CC_SHA256_Update(&hash, data + pos, static_cast<CC_LONG>(n));
#else
    hash.bytes(data + pos, n);
#endif
    pos += n;
  }
#if defined(__APPLE__)
  TileKey content;
  CC_SHA256_Final(content.data(), &hash);
#else
  auto content = hash.digest();
#endif
  Hash leaf;
  leaf.text("input");
  leaf.bytes(content.data(), content.size());
  auto key = leaf.digest();
  std::lock_guard<std::mutex> guard(mutex_);
  if (inputs_.size() >= std::min<std::uint64_t>(65536, limit_ / 256))
    inputs_.clear();
  inputs_[slice] = {owner, key};
  return key;
}
TileCache::Owner TileCache::find(const TileKey& key) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = index_.find(key);
  if (it == index_.end())
    return {};
  recent_.splice(recent_.begin(), recent_, it->second);
  return it->second->owner;
}
bool TileCache::clear() {
  std::lock_guard<std::mutex> guard(mutex_);
  const bool evicted = !index_.empty() || !inputs_.empty();
  index_.clear();
  recent_.clear();
  inputs_.clear();
  used_ = 0;
  return evicted;
}
void TileCache::store(const TileKey& key, const Owner& owner) {
  const auto size = owner->capacity();
  if (limit_ < 512 || size > limit_ - 512)
    return;
  const auto charge = size + 512;
  std::lock_guard<std::mutex> guard(mutex_);
  if (index_.count(key))
    return;
  while (used_ > limit_ - charge) {
    used_ -= recent_.back().charge;
    index_.erase(recent_.back().key);
    recent_.pop_back();
  }
  recent_.push_front({key, owner, charge});
  try {
    index_.emplace(key, recent_.begin());
  } catch (...) {
    recent_.pop_front();
    throw;
  }
  used_ += charge;
}

ResourceVector<TileKey> tile_keys(const ExecutionPlan& plan, Id block,
                                  const std::function<TileKey(Id)>& input_key,
                                  const CheckReuse& check) {
  const auto& p = plan.program();
  ResourceVector<TileKey> keys(p.tiles.size()), calls(p.calls.size());
  const auto& b = p.blocks[block];
  for (auto id : b.calls)
    calls[id] = call_key(plan, id);
  ResourceVector<Id> pending(p.tasks.size()), ready;
  for (auto id : b.tasks) {
    pending[id] = p.tasks[id].predecessors.size();
    if (!pending[id])
      ready.push_back(id);
  }
  while (!ready.empty()) {
    check();
    const auto id = ready.back();
    ready.pop_back();
    const auto& task = p.tasks[id];
    for (auto next : task.successors)
      if (!--pending[next])
        ready.push_back(next);
    const auto& placement = p.placements[task.placement];
    if (task.writes.empty() || placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host ||
        task.kind == physical::TaskKind::Effect ||
        !task.explicit_dependencies.empty())
      continue;
    Hash h;
    if (task.kind == physical::TaskKind::Copy) {
      h.text("copy");
      h.integer(p.tile_extent);
    } else {
      if (calls[task.call] == TileKey{})
        continue;
      h.bytes(calls[task.call].data(), calls[task.call].size());
      const auto& k = p.kernels[task.kernel];
      h.text(k.operation);
      h.text(k.symbol);
      h.text(k.contract);
      h.integer(k.abi_version);
    }
    h.text(placement.profile);
    h.text(placement.queue);
    h.integer(task.parameters.size());
    h.bytes(task.parameters.data(), task.parameters.size());
    for (auto n : task.scratch)
      h.integer(n);
    h.integer(task.schema_reads.size());
    for (auto vi : task.schema_reads) {
      const auto schema = p.schemas[p.values[vi].schema]->canonical();
      h.text({schema.data(), schema.size()});
    }
    h.integer(task.writes.size());
    for (auto id : task.writes) {
      Id port = physical::none;
      if (task.call != physical::none) {
        const auto& outputs = plan.calls()[task.call].outputs;
        auto it = std::find(outputs.begin(), outputs.end(), p.tiles[id].value);
        if (it != outputs.end())
          port = static_cast<Id>(it - outputs.begin());
      }
      view_key(&h, p, id, port, p.tiles[id].layout);
    }
    bool known = true;
    for (const auto& read : task.reads) {
      const auto& t = p.tiles[read.tile];
      if (t.domain != physical::Domain::Host) {
        known = false;
        break;
      }
      if (keys[read.tile] == TileKey{} && t.producer == physical::none)
        keys[read.tile] = input_key(read.tile);
      if (keys[read.tile] == TileKey{}) {
        known = false;
        break;
      }
      h.bytes(keys[read.tile].data(), keys[read.tile].size());
      view_key(&h, p, read.tile, read.port, read.layout);
    }
    if (known) {
      for (std::size_t i = 0; i < task.writes.size(); ++i) {
        auto output = h;
        output.integer(i);
        keys[task.writes[i]] = output.digest();
      }
    }
  }
  return keys;
}
}  // namespace ps::execution_internal
