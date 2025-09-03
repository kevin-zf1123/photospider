// FILE: src/ps_types.cpp (修改后)

#include "ps_types.hpp"
#include <algorithm>

namespace ps {

OpRegistry& OpRegistry::instance() {
    static OpRegistry inst;
    return inst;
}

// --- 修改: 实现新的 register_op 和 get_metadata ---

void OpRegistry::register_op(const std::string& type, const std::string& subtype, MonolithicOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    table_[key] = std::move(fn);
    metadata_table_[key] = meta; // 存储元数据
}

void OpRegistry::register_op(const std::string& type, const std::string& subtype, TileOpFunc fn, OpMetadata meta) {
    auto key = make_key(type, subtype);
    if (meta.tile_preference == TileSizePreference::UNDEFINED) {
        throw std::logic_error("Tiled operations must specify a TileSizePreference (MICRO or MACRO).");
    }
    table_[key] = std::move(fn);
    metadata_table_[key] = meta; // 存储元数据
}

std::optional<OpRegistry::OpVariant> OpRegistry::find(const std::string& type, const std::string& subtype) const {
    auto it = table_.find(make_key(type, subtype));
    if (it == table_.end()) return std::nullopt;
    return it->second;
}

std::optional<OpMetadata> OpRegistry::get_metadata(const std::string& type, const std::string& subtype) const {
    auto it = metadata_table_.find(make_key(type, subtype));
    if (it == metadata_table_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> OpRegistry::get_keys() const {
    std::vector<std::string> keys;
    keys.reserve(table_.size());
    for (const auto& pair : table_) {
        keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
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

} // namespace ps