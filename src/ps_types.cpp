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

} // namespace ps