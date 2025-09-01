// in: src/ps_types.cpp (OVERWRITE)
#include "ps_types.hpp"
#include <algorithm> // for std::sort

namespace ps {

OpRegistry& OpRegistry::instance() {
    static OpRegistry inst;
    return inst;
}

// NEW: 重载 register_op 以处理不同函数类型
void OpRegistry::register_op(const std::string& type, const std::string& subtype, MonolithicOpFunc fn) {
    table_[make_key(type, subtype)] = std::move(fn);
}

void OpRegistry::register_op(const std::string& type, const std::string& subtype, TileOpFunc fn) {
    table_[make_key(type, subtype)] = std::move(fn);
}

std::optional<OpRegistry::OpVariant> OpRegistry::find(const std::string& type, const std::string& subtype) const {
    auto it = table_.find(make_key(type, subtype));
    if (it == table_.end()) return std::nullopt;
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
    auto it = table_.find(key);
    if (it == table_.end()) return false;
    table_.erase(it);
    return true;
}

} // namespace ps