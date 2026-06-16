#include "ps_types.hpp"

#include <algorithm>
#include <stdexcept>

namespace ps {

OpRegistry& OpRegistry::instance() {
  static OpRegistry inst;
  return inst;
}

// --- 修改: 实现新的 register_op 和 get_metadata ---

void OpRegistry::register_op(const std::string& type,
                             const std::string& subtype, MonolithicOpFunc fn,
                             OpMetadata meta) {
  auto key = make_key(type, subtype);
  table_[key] = fn;
  metadata_table_[key] = meta;  // 存储元数据
  // Phase 1 bridge: also populate multi-impl table as HP monolithic
  impl_table_[key].monolithic_hp = std::move(fn);
  impl_table_[key].meta_hp = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_op(const std::string& type,
                             const std::string& subtype, TileOpFunc fn,
                             OpMetadata meta) {
  auto key = make_key(type, subtype);
  if (meta.tile_preference == TileSizePreference::UNDEFINED) {
    // Tiled 操作可以不指定偏好，默认为 UNDEFINED
  }
  table_[key] = fn;
  metadata_table_[key] = meta;  // 存储元数据
  // Phase 1 bridge: also populate multi-impl table as HP tiled
  impl_table_[key].tiled_hp = std::move(fn);
  impl_table_[key].meta_hp = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

std::optional<OpRegistry::OpVariant> OpRegistry::find(
    const std::string& type, const std::string& subtype) const {
  auto it = table_.find(make_key(type, subtype));
  if (it == table_.end())
    return std::nullopt;
  return it->second;
}

std::optional<OpMetadata> OpRegistry::get_metadata(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = metadata_table_.find(key);
  if (it != metadata_table_.end())
    return it->second;
  // Fallback to new multi-impl table
  auto it2 = impl_table_.find(key);
  if (it2 == impl_table_.end())
    return std::nullopt;
  if (it2->second.meta_hp)
    return *(it2->second.meta_hp);
  if (it2->second.meta_rt)
    return *(it2->second.meta_rt);
  return std::nullopt;
}

std::vector<std::string> OpRegistry::get_keys() const {
  std::vector<std::string> keys;
  keys.reserve(table_.size() + impl_table_.size());
  for (const auto& pair : table_)
    keys.push_back(pair.first);
  for (const auto& pair : impl_table_)
    keys.push_back(pair.first);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

std::vector<std::string> OpRegistry::get_combined_keys() const {
  // Start with keys that have multi-impl entries (canonical combined ops)
  std::vector<std::string> combined;
  combined.reserve(impl_table_.size() + table_.size());
  for (const auto& kv : impl_table_)
    combined.push_back(kv.first);
  // Add legacy-only keys (not present in impl table)
  for (const auto& kv : table_) {
    const auto& key = kv.first;
    if (impl_table_.count(key) == 0) {
      // If this legacy key looks like an alias ending with "_tiled" and the
      // base exists, skip it
      auto pos = key.rfind(':');
      if (pos != std::string::npos) {
        std::string type = key.substr(0, pos);
        std::string subtype = key.substr(pos + 1);
        if (subtype.size() > 6 &&
            subtype.rfind("_tiled") == subtype.size() - 6) {
          std::string base_key =
              type + ":" + subtype.substr(0, subtype.size() - 6);
          if (impl_table_.count(base_key) || table_.count(base_key)) {
            continue;  // collapse alias under base op
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

bool OpRegistry::unregister_op(const std::string& type,
                               const std::string& subtype) {
  return unregister_key(make_key(type, subtype));
}

bool OpRegistry::unregister_key(const std::string& key) {
  const bool removed_metadata = metadata_table_.erase(key) > 0;
  const bool removed_legacy = table_.erase(key) > 0;
  const bool removed_impls = impl_table_.erase(key) > 0;
  return removed_metadata || removed_legacy || removed_impls;
}

// -------------------------
// Phase 1 scaffolding below
// -------------------------

void OpRegistry::register_op_hp_monolithic(const std::string& type,
                                           const std::string& subtype,
                                           MonolithicOpFunc fn,
                                           OpMetadata meta) {
  auto key = make_key(type, subtype);
  impl_table_[key].monolithic_hp = std::move(fn);
  impl_table_[key].meta_hp = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_op_hp_tiled(const std::string& type,
                                      const std::string& subtype, TileOpFunc fn,
                                      OpMetadata meta) {
  auto key = make_key(type, subtype);
  impl_table_[key].tiled_hp = std::move(fn);
  impl_table_[key].meta_hp = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_op_rt_tiled(const std::string& type,
                                      const std::string& subtype, TileOpFunc fn,
                                      OpMetadata meta) {
  auto key = make_key(type, subtype);
  impl_table_[key].tiled_rt = std::move(fn);
  impl_table_[key].meta_rt = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_dirty_propagator(const std::string& type,
                                           const std::string& subtype,
                                           DirtyRoiPropFunc fn) {
  auto key = make_key(type, subtype);
  impl_table_[key].dirty_propagator = std::move(fn);
}

void OpRegistry::register_forward_propagator(const std::string& type,
                                             const std::string& subtype,
                                             ForwardRoiPropFunc fn) {
  auto key = make_key(type, subtype);
  impl_table_[key].forward_propagator = std::move(fn);
}

void OpRegistry::register_dependency_builder(const std::string& type,
                                             const std::string& subtype,
                                             DependencyLutBuilder fn,
                                             bool mark_data_dependent) {
  auto key = make_key(type, subtype);
  impl_table_[key].dependency_builder = std::move(fn);
  if (mark_data_dependent) {
    impl_table_[key].data_dependent = true;
  }
}

std::optional<OpRegistry::OpVariant> OpRegistry::resolve_for_intent(
    const std::string& type, const std::string& subtype,
    ComputeIntent intent) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it == impl_table_.end()) {
    // fallback to legacy single-impl table
    return find(type, subtype);
  }
  const auto& impls = it->second;
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      if (impls.monolithic_hp)
        return OpVariant{*impls.monolithic_hp};
      if (impls.tiled_hp)
        return OpVariant{*impls.tiled_hp};
      break;
    case ComputeIntent::RealTimeUpdate:
      if (impls.tiled_rt)
        return OpVariant{*impls.tiled_rt};
      if (impls.tiled_hp)
        return OpVariant{*impls.tiled_hp};
      break;
    default:
      break;
  }
  return find(type, subtype);
}

DirtyRoiPropFunc OpRegistry::get_dirty_propagator(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.dirty_propagator) {
      return *(it->second.dirty_propagator);
    }
  }
  static const DirtyRoiPropFunc kIdentity =
      [](const Node&, const cv::Rect& roi, const GraphModel&) { return roi; };
  return kIdentity;
}

ForwardRoiPropFunc OpRegistry::get_forward_propagator(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.forward_propagator) {
      return *(it->second.forward_propagator);
    }
  }
  static const ForwardRoiPropFunc kIdentity =
      [](const Node&, const cv::Rect& roi, const GraphModel&, const cv::Size&,
         const cv::Size&) { return roi; };
  return kIdentity;
}

PropagationContractStatus OpRegistry::dirty_propagation_contract_status(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end() && it->second.dirty_propagator) {
    return PropagationContractStatus::Explicit;
  }
  return PropagationContractStatus::LegacyIdentityFallback;
}

PropagationContractStatus OpRegistry::forward_propagation_contract_status(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end() && it->second.forward_propagator) {
    return PropagationContractStatus::Explicit;
  }
  return PropagationContractStatus::LegacyIdentityFallback;
}

std::optional<DependencyLutBuilder> OpRegistry::get_dependency_builder(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.dependency_builder) {
      return it->second.dependency_builder;
    }
  }
  return std::nullopt;
}

bool OpRegistry::is_data_dependent(const std::string& type,
                                   const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it != impl_table_.end()) {
    if (it->second.data_dependent)
      return true;
    if (it->second.meta_hp && it->second.meta_hp->data_dependent)
      return true;
    if (it->second.meta_rt && it->second.meta_rt->data_dependent)
      return true;
  }
  auto meta = get_metadata(type, subtype);
  return meta && meta->data_dependent;
}

const OpRegistry::OpImplementations* OpRegistry::get_implementations(
    const std::string& type, const std::string& subtype) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it == impl_table_.end()) {
    return nullptr;
  }
  return &it->second;
}

// =============================================================================
// [M3.1 新增] 多设备实现注册与检索方法
// =============================================================================

void OpRegistry::register_impl(const std::string& type,
                               const std::string& subtype, Device device,
                               MonolithicOpFunc fn, OpMetadata meta) {
  auto key = make_key(type, subtype);
  // 设置元数据中的设备偏好
  meta.device_preference = device;

  OpImplementation impl;
  impl.func = std::move(fn);
  impl.metadata = meta;

  impl_table_[key].device_impls.push_back(std::move(impl));
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;

  // 同时更新传统表以保持向后兼容
  if (device == Device::CPU) {
    if (!impl_table_[key].monolithic_hp) {
      impl_table_[key].monolithic_hp =
          std::get<MonolithicOpFunc>(impl_table_[key].device_impls.back().func);
      impl_table_[key].meta_hp = meta;
    }
  }
}

void OpRegistry::register_impl(const std::string& type,
                               const std::string& subtype, Device device,
                               TileOpFunc fn, OpMetadata meta) {
  auto key = make_key(type, subtype);
  // 设置元数据中的设备偏好
  meta.device_preference = device;

  OpImplementation impl;
  impl.func = std::move(fn);
  impl.metadata = meta;

  impl_table_[key].device_impls.push_back(std::move(impl));
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;

  // 同时更新传统表以保持向后兼容
  if (device == Device::CPU) {
    if (!impl_table_[key].tiled_hp) {
      impl_table_[key].tiled_hp =
          std::get<TileOpFunc>(impl_table_[key].device_impls.back().func);
      impl_table_[key].meta_hp = meta;
    }
  }
}

std::vector<const OpImplementation*> OpRegistry::get_implementations_by_device(
    const std::string& type, const std::string& subtype, Device device) const {
  std::vector<const OpImplementation*> result;
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it == impl_table_.end()) {
    return result;
  }

  for (const auto& impl : it->second.device_impls) {
    if (impl.metadata.device_preference == device) {
      result.push_back(&impl);
    }
  }
  return result;
}

std::vector<const OpImplementation*> OpRegistry::get_all_implementations(
    const std::string& type, const std::string& subtype) const {
  std::vector<const OpImplementation*> result;
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it == impl_table_.end()) {
    return result;
  }

  for (const auto& impl : it->second.device_impls) {
    result.push_back(&impl);
  }
  return result;
}

const OpImplementation* OpRegistry::select_best_implementation(
    const std::string& type, const std::string& subtype,
    const std::vector<Device>& available_devices, ComputeIntent intent) const {
  auto key = make_key(type, subtype);
  auto it = impl_table_.find(key);
  if (it == impl_table_.end()) {
    return nullptr;
  }

  // 收集所有可用设备上的实现
  std::vector<const OpImplementation*> candidates;
  for (const auto& impl : it->second.device_impls) {
    Device impl_device = impl.metadata.device_preference;
    // 检查实现的设备是否在可用设备列表中
    if (std::find(available_devices.begin(), available_devices.end(),
                  impl_device) != available_devices.end()) {
      candidates.push_back(&impl);
    }
  }

  if (candidates.empty()) {
    return nullptr;
  }

  // 根据 ComputeIntent 和 cost_score 排序选择最优实现
  // HP 模式: 优先 GPU > CPU (Monolithic) > CPU (Tiled)，同设备按 cost_score
  // RT 模式: 优先 CPU (Tiled) > GPU，同设备按 cost_score
  auto compare_impl = [intent](const OpImplementation* a,
                               const OpImplementation* b) {
    Device da = a->metadata.device_preference;
    Device db = b->metadata.device_preference;

    // 设备优先级映射
    auto device_priority = [intent](Device d, bool is_tiled) -> int {
      switch (intent) {
        case ComputeIntent::GlobalHighPrecision:
          // HP: GPU > CPU
          if (d == Device::GPU_METAL || d == Device::GPU_CUDA)
            return 0;
          if (d == Device::ASIC_NPU)
            return 1;
          return 2;  // CPU
        case ComputeIntent::RealTimeUpdate:
          // RT: CPU Tiled > GPU (低延迟优先)
          if (d == Device::CPU && is_tiled)
            return 0;
          if (d == Device::GPU_METAL || d == Device::GPU_CUDA)
            return 1;
          return 2;
        default:
          return 99;
      }
    };

    int prio_a = device_priority(da, a->is_tiled());
    int prio_b = device_priority(db, b->is_tiled());

    if (prio_a != prio_b) {
      return prio_a < prio_b;  // 优先级小的排前面
    }

    // 同优先级按 cost_score 排序
    return a->metadata.cost_score < b->metadata.cost_score;
  };

  // 找出最优实现
  auto best_it =
      std::min_element(candidates.begin(), candidates.end(), compare_impl);
  return *best_it;
}

}  // namespace ps
