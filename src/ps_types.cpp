#include "ps_types.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ps {

namespace {

/**
 * @brief Thread-local capture currently observing OpRegistry registrations.
 *
 * @note Plugin loading invokes registration on the calling thread, so a
 * thread-local pointer avoids adding process-global registry state while still
 * allowing nested callers to restore the previous capture.
 */
thread_local OpRegistry::RegistrationCapture* active_registration_capture =
    nullptr;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Sorts and deduplicates captured canonical operation keys.
 *
 * @param keys Mutable list of keys captured during one registration call.
 * @throws std::bad_alloc if sorting or unique operations allocate internally.
 * @note Stable sorted output keeps plugin unload metadata deterministic.
 */
void sort_unique_keys(std::vector<std::string>& keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

/**
 * @brief Maps one implementation to an intent-specific device priority.
 *
 * @param intent Compute path selecting the HP or RT priority policy.
 * @param device Candidate implementation device.
 * @param is_tiled Whether the candidate uses the tiled callback shape.
 * @return Lower values represent higher registry selection priority.
 * @throws Nothing.
 * @note GlobalHighPrecision prefers GPU backends, then accelerator backends,
 * then CPU. RealTimeUpdate prefers tiled CPU callbacks before GPU backends,
 * preserving the existing low-latency RT policy.
 */
int implementation_device_priority(ComputeIntent intent, Device device,
                                   bool is_tiled) {
  switch (intent) {
    case ComputeIntent::GlobalHighPrecision:
      if (device == Device::GPU_METAL || device == Device::GPU_CUDA) {
        return 0;
      }
      if (device == Device::ASIC_NPU) {
        return 1;
      }
      return 2;
    case ComputeIntent::RealTimeUpdate:
      if (device == Device::CPU && is_tiled) {
        return 0;
      }
      if (device == Device::GPU_METAL || device == Device::GPU_CUDA) {
        return 1;
      }
      return 2;
    default:
      return 99;
  }
}

/**
 * @brief Orders two implementation candidates by registry policy.
 *
 * @param lhs Left candidate borrowed from an OpRegistry implementation vector.
 * @param rhs Right candidate borrowed from an OpRegistry implementation vector.
 * @param intent Compute path selecting the HP or RT priority policy.
 * @return True when `lhs` should sort before `rhs`.
 * @throws Nothing.
 * @note Device priority is compared before `cost_score`; equal priorities and
 * equal costs preserve `std::min_element`'s first-seen candidate selection.
 */
bool implementation_less_for_intent(const OpImplementation* lhs,
                                    const OpImplementation* rhs,
                                    ComputeIntent intent) {
  const Device lhs_device = lhs->metadata.device_preference;
  const Device rhs_device = rhs->metadata.device_preference;
  const int lhs_priority =
      implementation_device_priority(intent, lhs_device, lhs->is_tiled());
  const int rhs_priority =
      implementation_device_priority(intent, rhs_device, rhs->is_tiled());
  if (lhs_priority != rhs_priority) {
    return lhs_priority < rhs_priority;
  }
  return lhs->metadata.cost_score < rhs->metadata.cost_score;
}

}  // namespace

OpRegistry& OpRegistry::instance() {
  static OpRegistry inst;
  return inst;
}

OpRegistry::RegistryEntrySnapshot OpRegistry::snapshot_entry(
    const std::string& key) const {
  RegistryEntrySnapshot snapshot;
  if (auto it = table_.find(key); it != table_.end()) {
    snapshot.legacy_op = it->second;
  }
  if (auto it = metadata_table_.find(key); it != metadata_table_.end()) {
    snapshot.metadata = it->second;
  }
  if (auto it = impl_table_.find(key); it != impl_table_.end()) {
    snapshot.implementations = it->second;
  }
  return snapshot;
}

void OpRegistry::restore_entry(const std::string& key,
                               const RegistryEntrySnapshot& snapshot) {
  if (snapshot.legacy_op) {
    table_[key] = *snapshot.legacy_op;
  } else {
    table_.erase(key);
  }
  if (snapshot.metadata) {
    metadata_table_[key] = *snapshot.metadata;
  } else {
    metadata_table_.erase(key);
  }
  if (snapshot.implementations) {
    impl_table_[key] = *snapshot.implementations;
  } else {
    impl_table_.erase(key);
  }
}

void OpRegistry::capture_key_before_mutation(const std::string& key) {
  if (!active_registration_capture) {
    return;
  }
  if (active_registration_capture->previous_entries.count(key) == 0) {
    active_registration_capture->previous_entries.emplace(key,
                                                          snapshot_entry(key));
  }
  active_registration_capture->registered_keys.push_back(key);
}

void OpRegistry::capture_registration(const std::function<void()>& registration,
                                      RegistrationCapture& capture) {
  capture.registered_keys.clear();
  capture.previous_entries.clear();

  auto* previous_capture = active_registration_capture;
  active_registration_capture = &capture;
  auto finish_capture = [&]() {
    active_registration_capture = previous_capture;
    sort_unique_keys(capture.registered_keys);
  };

  try {
    registration();
  } catch (...) {
    finish_capture();
    throw;
  }
  finish_capture();
}

void OpRegistry::restore_registration_capture(
    const RegistrationCapture& capture) {
  for (const auto& key : capture.registered_keys) {
    auto snapshot_it = capture.previous_entries.find(key);
    if (snapshot_it != capture.previous_entries.end()) {
      restore_entry(key, snapshot_it->second);
    }
  }
}

/** @copydoc OpRegistry::restore_entry_noexcept */
bool OpRegistry::restore_entry_noexcept(
    const std::string& key, RegistryEntrySnapshot& snapshot) noexcept {
  static_assert(std::is_nothrow_swappable_v<OpVariant>);
  static_assert(std::is_nothrow_swappable_v<OpMetadata>);
  static_assert(std::is_nothrow_swappable_v<OpImplementations>);

  bool changed = false;
  const auto restore_table = [&key, &changed](auto& table,
                                              auto& previous) noexcept {
    auto active = table.find(key);
    if (previous) {
      if (active != table.end()) {
        using std::swap;
        swap(active->second, *previous);
        changed = true;
      }
      return;
    }
    if (active != table.end()) {
      table.erase(active);
      changed = true;
    }
  };

  restore_table(table_, snapshot.legacy_op);
  restore_table(metadata_table_, snapshot.metadata);
  restore_table(impl_table_, snapshot.implementations);
  return changed;
}

/**
 * @brief Publishes a prepared registry through allocation-free table swaps.
 * @param other Host-owned shadow registry whose complete state is exchanged.
 * @return Nothing.
 * @throws Nothing; all member containers provide noexcept swap here.
 * @note Plugin-load commit retains both relevant library lifetimes until
 * callable objects in the swapped-out registry have been destroyed.
 */
void OpRegistry::swap_state(OpRegistry& other) noexcept {
  table_.swap(other.table_);
  metadata_table_.swap(other.metadata_table_);
  impl_table_.swap(other.impl_table_);
}

// --- 修改: 实现新的 register_op 和 get_metadata ---

void OpRegistry::register_op(const std::string& type,
                             const std::string& subtype, MonolithicOpFunc fn,
                             OpMetadata meta) {
  auto key = make_key(type, subtype);
  capture_key_before_mutation(key);
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
  capture_key_before_mutation(key);
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
  capture_key_before_mutation(key);
  impl_table_[key].monolithic_hp = std::move(fn);
  impl_table_[key].meta_hp = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_op_hp_tiled(const std::string& type,
                                      const std::string& subtype, TileOpFunc fn,
                                      OpMetadata meta) {
  auto key = make_key(type, subtype);
  capture_key_before_mutation(key);
  impl_table_[key].tiled_hp = std::move(fn);
  impl_table_[key].meta_hp = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_op_rt_tiled(const std::string& type,
                                      const std::string& subtype, TileOpFunc fn,
                                      OpMetadata meta) {
  auto key = make_key(type, subtype);
  capture_key_before_mutation(key);
  impl_table_[key].tiled_rt = std::move(fn);
  impl_table_[key].meta_rt = meta;
  impl_table_[key].data_dependent =
      impl_table_[key].data_dependent || meta.data_dependent;
}

void OpRegistry::register_dirty_propagator(const std::string& type,
                                           const std::string& subtype,
                                           DirtyRoiPropFunc fn) {
  auto key = make_key(type, subtype);
  capture_key_before_mutation(key);
  impl_table_[key].dirty_propagator = std::move(fn);
}

void OpRegistry::register_forward_propagator(const std::string& type,
                                             const std::string& subtype,
                                             ForwardRoiPropFunc fn) {
  auto key = make_key(type, subtype);
  capture_key_before_mutation(key);
  impl_table_[key].forward_propagator = std::move(fn);
}

void OpRegistry::register_dependency_builder(const std::string& type,
                                             const std::string& subtype,
                                             DependencyLutBuilder fn,
                                             bool mark_data_dependent) {
  auto key = make_key(type, subtype);
  capture_key_before_mutation(key);
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
  capture_key_before_mutation(key);
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
  capture_key_before_mutation(key);
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
  return select_best_implementation(
      type, subtype, available_devices, intent,
      std::function<bool(const OpImplementation&)>{});
}

const OpImplementation* OpRegistry::select_best_implementation(
    const std::string& type, const std::string& subtype,
    const std::vector<Device>& available_devices, ComputeIntent intent,
    const std::function<bool(const OpImplementation&)>& candidate_filter)
    const {  // NOLINT(whitespace/indent_namespace)
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
      if (candidate_filter && !candidate_filter(impl)) {
        continue;
      }
      candidates.push_back(&impl);
    }
  }

  if (candidates.empty()) {
    return nullptr;
  }

  // 找出最优实现
  auto best_it = std::min_element(
      candidates.begin(), candidates.end(),
      [intent](const OpImplementation* lhs, const OpImplementation* rhs) {
        return implementation_less_for_intent(lhs, rhs, intent);
      });
  return *best_it;
}

}  // namespace ps
