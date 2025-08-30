#include "ps_types.hpp"

namespace ps {

OpRegistry& OpRegistry::instance() {
    static OpRegistry inst;
    return inst;
}

void OpRegistry::register_op(const std::string& type, const std::string& subtype, OpFunc fn) {
    table_[make_key(type, subtype)] = std::move(fn);
}

std::optional<OpFunc> OpRegistry::find(const std::string& type, const std::string& subtype) const {
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
    // Sorting makes the "diff" logic later more reliable and readable
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
