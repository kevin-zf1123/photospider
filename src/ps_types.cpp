#include "ps_types.hpp"
#include <algorithm>
#include <stdexcept>

namespace ps {

OpRegistry& OpRegistry::instance() {
    static OpRegistry inst;
    return inst;
}

// --- 修改: 实现新的 register_op 和 get_metadata ---

void OpRegistry::register_op(const std::string& type, const std::string& subtype, MonolithicOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    table_[key] = fn;
    metadata_table_[key] = meta; // 存储元数据
    // Phase 1 bridge: also populate multi-impl table as HP monolithic
    impl_table_[key].monolithic_hp = std::move(fn);
    impl_table_[key].meta_hp = meta;
}

void OpRegistry::register_op(const std::string& type, const std::string& subtype, TileOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    if (meta.tile_preference == TileSizePreference::UNDEFINED) {
        // Tiled 操作可以不指定偏好，默认为 UNDEFINED
    }
    table_[key] = fn;
    metadata_table_[key] = meta; // 存储元数据
    // Phase 1 bridge: also populate multi-impl table as HP tiled
    impl_table_[key].tiled_hp = std::move(fn);
    impl_table_[key].meta_hp = meta;
}

std::optional<OpRegistry::OpVariant> OpRegistry::find(const std::string& type, const std::string& subtype) const {
    auto it = table_.find(make_key(type, subtype));
    if (it == table_.end()) return std::nullopt;
    return it->second;
}

std::optional<OpMetadata> OpRegistry::get_metadata(const std::string& type, const std::string& subtype) const {
    auto key = make_key(type, subtype);
    auto it = metadata_table_.find(key);
    if (it != metadata_table_.end()) return it->second;
    // Fallback to new multi-impl table
    auto it2 = impl_table_.find(key);
    if (it2 == impl_table_.end()) return std::nullopt;
    if (it2->second.meta_hp) return *(it2->second.meta_hp);
    if (it2->second.meta_rt) return *(it2->second.meta_rt);
    return std::nullopt;
}

std::vector<std::string> OpRegistry::get_keys() const {
    std::vector<std::string> keys;
    keys.reserve(table_.size() + impl_table_.size());
    for (const auto& pair : table_) keys.push_back(pair.first);
    for (const auto& pair : impl_table_) keys.push_back(pair.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::vector<std::string> OpRegistry::get_combined_keys() const {
    // Start with keys that have multi-impl entries (canonical combined ops)
    std::vector<std::string> combined;
    combined.reserve(impl_table_.size() + table_.size());
    for (const auto& kv : impl_table_) combined.push_back(kv.first);
    // Add legacy-only keys (not present in impl table)
    for (const auto& kv : table_) {
        const auto& key = kv.first;
        if (impl_table_.count(key) == 0) {
            // If this legacy key looks like an alias ending with "_tiled" and the base exists, skip it
            auto pos = key.rfind(':');
            if (pos != std::string::npos) {
                std::string type = key.substr(0, pos);
                std::string subtype = key.substr(pos + 1);
                if (subtype.size() > 6 && subtype.rfind("_tiled") == subtype.size() - 6) {
                    std::string base_key = type + ":" + subtype.substr(0, subtype.size() - 6);
                    if (impl_table_.count(base_key) || table_.count(base_key)) {
                        continue; // collapse alias under base op
                    }
                }
            }
            combined.push_back(key);
        }
    }
    std::sort(combined.begin(), combined.end());
    combined.erase(std::unique(combined.begin(), combined.end()), combined.end());
    return combined;
}

bool OpRegistry::unregister_op(const std::string& type, const std::string& subtype) {
    return unregister_key(make_key(type, subtype));
}

bool OpRegistry::unregister_key(const std::string& key) {
    metadata_table_.erase(key); // 同时清理元数据
    auto it = table_.find(key);
    if (it == table_.end()) return false;
    table_.erase(it);
    return true;
}

// -------------------------
// Phase 1 scaffolding below
// -------------------------

void OpRegistry::register_op_hp_monolithic(const std::string& type, const std::string& subtype, MonolithicOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    impl_table_[key].monolithic_hp = std::move(fn);
    impl_table_[key].meta_hp = meta;
}

void OpRegistry::register_op_hp_tiled(const std::string& type, const std::string& subtype, TileOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    impl_table_[key].tiled_hp = std::move(fn);
    impl_table_[key].meta_hp = meta;
}

void OpRegistry::register_op_rt_tiled(const std::string& type, const std::string& subtype, TileOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    impl_table_[key].tiled_rt = std::move(fn);
    impl_table_[key].meta_rt = meta;
}

std::optional<OpRegistry::OpVariant> OpRegistry::resolve_for_intent(const std::string& type, const std::string& subtype, ComputeIntent intent) const {
    auto key = make_key(type, subtype);
    auto it = impl_table_.find(key);
    if (it == impl_table_.end()) {
        // fallback to legacy single-impl table
        return find(type, subtype);
    }
    const auto& impls = it->second;
    switch (intent) {
        case ComputeIntent::GlobalHighPrecision:
            if (impls.monolithic_hp) return OpVariant{*impls.monolithic_hp};
            if (impls.tiled_hp) return OpVariant{*impls.tiled_hp};
            break;
        case ComputeIntent::RealTimeUpdate:
            if (impls.tiled_rt) return OpVariant{*impls.tiled_rt};
            if (impls.tiled_hp) return OpVariant{*impls.tiled_hp};
            break;
        default:
            break;
    }
    return find(type, subtype);
}

} // namespace ps
