/**
 * @file test_op_registry_m31.cpp
 * @brief M3.1 里程碑测试：OpRegistry 多设备实现支持
 *
 * 验收标准：
 * - 单元测试能注册同一算子的 CPU 和 Metal 版本
 * - 能按 Metadata 检索
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)

namespace ps {
namespace {

// 测试用的虚拟算子函数
NodeOutput dummy_cpu_op(const Node&, const std::vector<const NodeOutput*>&) {
  NodeOutput output;
  output.debug.compute_device = "CPU";
  return output;
}

NodeOutput dummy_metal_op(const Node&, const std::vector<const NodeOutput*>&) {
  NodeOutput output;
  output.debug.compute_device = "GPU_METAL";
  return output;
}

NodeOutput dummy_cuda_op(const Node&, const std::vector<const NodeOutput*>&) {
  NodeOutput output;
  output.debug.compute_device = "GPU_CUDA";
  return output;
}

void dummy_tiled_cpu_op(const Node&, const OutputTile&,
                        const std::vector<InputTile>&) {
  // Tiled CPU 实现
}

void dummy_tiled_metal_op(const Node&, const OutputTile&,
                          const std::vector<InputTile>&) {
  // Tiled Metal 实现
}

/**
 * @brief Asserts the five execution-authority metadata fields of one candidate.
 *
 * @param implementation Coherent scalar implementation selected by registry.
 * @param reentrant Expected reentrancy policy.
 * @param maximum_parallelism Expected per-identity concurrency cap.
 * @param retained_memory_bytes Expected declared retained memory.
 * @param scratch_bytes Expected declared scratch memory.
 * @param exclusive_key Expected process-exclusive context key.
 * @return Nothing; GoogleTest records every mismatch.
 * @throws Nothing directly.
 * @note Device, cost, callback shape, and identity are asserted by callers so
 * this helper remains focused on the fields consumed by ExecutionService.
 */
void expect_execution_metadata(const OpImplementation& implementation,
                               bool reentrant,
                               std::uint32_t maximum_parallelism,
                               std::uint64_t retained_memory_bytes,
                               std::uint64_t scratch_bytes,
                               const std::string& exclusive_key) {
  EXPECT_EQ(implementation.metadata.reentrant, reentrant);
  EXPECT_EQ(implementation.metadata.maximum_parallelism, maximum_parallelism);
  EXPECT_EQ(implementation.metadata.retained_memory_bytes,
            retained_memory_bytes);
  EXPECT_EQ(implementation.metadata.scratch_bytes, scratch_bytes);
  EXPECT_EQ(implementation.metadata.exclusive_key, exclusive_key);
}

/**
 * @brief Verifies scalar callback, metadata, and identity stay atomic across
 * both HP registration orders.
 *
 * @param subtype Unique operation subtype owned by this verification.
 * @param monolithic_first True to publish monolithic before tiled; false to
 * reverse the order.
 * @return Nothing; GoogleTest records selection and invocation failures.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note Ordinary HP selection must choose the monolithic slot after both
 * registrations, while a tiled-only filter must choose the tiled slot. The
 * first slot's identity must survive publication of the second slot.
 */
void verify_atomic_scalar_slots(const std::string& subtype,
                                bool monolithic_first) {
  constexpr const char* kType = "issue82_atomic_scalar";
  auto& registry = OpRegistry::instance();
  const std::string key = make_key(kType, subtype);
  registry.unregister_key(key);

  auto monolithic_invocations = std::make_shared<int>(0);
  auto tiled_invocations = std::make_shared<int>(0);
  MonolithicOpFunc monolithic = [monolithic_invocations](
                                    const Node&,
                                    const std::vector<const NodeOutput*>&) {
    ++*monolithic_invocations;
    NodeOutput output;
    output.debug.compute_device = "atomic-monolithic";
    return output;
  };
  TileOpFunc tiled = [tiled_invocations](const Node&, const OutputTile&,
                                         const std::vector<InputTile>&) {
    ++*tiled_invocations;
  };

  OpMetadata monolithic_metadata;
  monolithic_metadata.device_preference = Device::CPU;
  monolithic_metadata.reentrant = false;
  monolithic_metadata.maximum_parallelism = 1U;
  monolithic_metadata.retained_memory_bytes = 101U;
  monolithic_metadata.scratch_bytes = 202U;
  monolithic_metadata.exclusive_key = "atomic-monolithic-context";
  monolithic_metadata.cost_score = 11;

  OpMetadata tiled_metadata;
  tiled_metadata.device_preference = Device::CPU;
  tiled_metadata.reentrant = true;
  tiled_metadata.maximum_parallelism = 4U;
  tiled_metadata.retained_memory_bytes = 303U;
  tiled_metadata.scratch_bytes = 404U;
  tiled_metadata.exclusive_key = "atomic-tiled-context";
  tiled_metadata.cost_score = 22;

  const auto register_monolithic = [&]() {
    registry.register_op_hp_monolithic(kType, subtype, monolithic,
                                       monolithic_metadata);
  };
  const auto register_tiled = [&]() {
    registry.register_op_hp_tiled(kType, subtype, tiled, tiled_metadata);
  };
  if (monolithic_first) {
    register_monolithic();
  } else {
    register_tiled();
  }
  const auto first = registry.select_implementation(
      kType, subtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(first.has_value());
  ASSERT_NE(first->implementation_identity, 0U);
  const std::uint64_t first_identity = first->implementation_identity;

  if (monolithic_first) {
    register_tiled();
  } else {
    register_monolithic();
  }

  const auto selected_monolithic = registry.select_implementation(
      kType, subtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  const auto selected_tiled = registry.select_implementation(
      kType, subtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision,
      [](const OpImplementation& candidate) { return candidate.is_tiled(); });
  ASSERT_TRUE(selected_monolithic.has_value());
  ASSERT_TRUE(selected_tiled.has_value());
  ASSERT_TRUE(selected_monolithic->is_monolithic());
  ASSERT_TRUE(selected_tiled->is_tiled());
  EXPECT_NE(selected_monolithic->implementation_identity,
            selected_tiled->implementation_identity);
  EXPECT_EQ(monolithic_first ? selected_monolithic->implementation_identity
                             : selected_tiled->implementation_identity,
            first_identity);

  expect_execution_metadata(*selected_monolithic, false, 1U, 101U, 202U,
                            "atomic-monolithic-context");
  expect_execution_metadata(*selected_tiled, true, 4U, 303U, 404U,
                            "atomic-tiled-context");
  EXPECT_EQ(selected_monolithic->metadata.cost_score, 11);
  EXPECT_EQ(selected_tiled->metadata.cost_score, 22);

  Node node;
  const NodeOutput monolithic_output =
      std::get<MonolithicOpFunc>(selected_monolithic->func)(node, {});
  const OutputTile output_tile;
  const std::vector<InputTile> input_tiles;
  std::get<TileOpFunc>(selected_tiled->func)(node, output_tile, input_tiles);
  EXPECT_EQ(monolithic_output.debug.compute_device, "atomic-monolithic");
  EXPECT_EQ(*monolithic_invocations, 1);
  EXPECT_EQ(*tiled_invocations, 1);
  registry.unregister_key(key);
}

class OpRegistryM31Test : public ::testing::Test {
 protected:
  void SetUp() override {
    // 每个测试前清理注册表（注意：实际实现中可能需要添加清理方法）
    // 这里我们使用唯一的类型名来避免冲突
  }

  void TearDown() override {
    // 清理测试注册的算子
  }
};

/**
 * @brief Proves a later tiled registration cannot overwrite monolithic
 * scheduling metadata or identity.
 * @return Nothing; GoogleTest records selection, metadata, or invocation
 * failures.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note The helper unregisters its unique operation key after verification.
 */
TEST_F(OpRegistryM31Test, ScalarSlotsStayAtomicWhenMonolithicRegistersFirst) {
  verify_atomic_scalar_slots("monolithic_then_tiled", true);
}

/**
 * @brief Proves a later monolithic registration cannot overwrite tiled
 * scheduling metadata or identity.
 * @return Nothing; GoogleTest records selection, metadata, or invocation
 * failures.
 * @throws Registry allocation or callback-copy exceptions unchanged.
 * @note The helper unregisters its unique operation key after verification.
 */
TEST_F(OpRegistryM31Test, ScalarSlotsStayAtomicWhenTiledRegistersFirst) {
  verify_atomic_scalar_slots("tiled_then_monolithic", false);
}

// 测试：注册同一算子的 CPU 和 Metal 版本
TEST_F(OpRegistryM31Test, RegisterMultiDeviceImplementations) {
  auto& registry = OpRegistry::instance();

  // 注册 CPU 版本
  OpMetadata cpu_meta;
  cpu_meta.device_preference = Device::CPU;
  cpu_meta.cost_score = 100;
  registry.register_impl("m31_test", "gaussian_blur", Device::CPU, dummy_cpu_op,
                         cpu_meta);

  // 注册 Metal 版本（更低的 cost_score 表示更高优先级）
  OpMetadata metal_meta;
  metal_meta.device_preference = Device::GPU_METAL;
  metal_meta.cost_score = 50;  // GPU 更优
  registry.register_impl("m31_test", "gaussian_blur", Device::GPU_METAL,
                         dummy_metal_op, metal_meta);

  // 验证两个版本都已注册
  auto all_impls =
      registry.get_all_implementations("m31_test", "gaussian_blur");
  ASSERT_EQ(all_impls.size(), 2);
}

// 测试：按设备检索实现
TEST_F(OpRegistryM31Test, GetImplementationsByDevice) {
  auto& registry = OpRegistry::instance();

  // 注册多个设备版本
  OpMetadata cpu_meta;
  cpu_meta.device_preference = Device::CPU;
  cpu_meta.cost_score = 100;
  registry.register_impl("m31_test", "invert", Device::CPU, dummy_cpu_op,
                         cpu_meta);

  OpMetadata metal_meta;
  metal_meta.device_preference = Device::GPU_METAL;
  metal_meta.cost_score = 50;
  registry.register_impl("m31_test", "invert", Device::GPU_METAL,
                         dummy_metal_op, metal_meta);

  OpMetadata cuda_meta;
  cuda_meta.device_preference = Device::GPU_CUDA;
  cuda_meta.cost_score = 60;
  registry.register_impl("m31_test", "invert", Device::GPU_CUDA, dummy_cuda_op,
                         cuda_meta);

  // 按设备检索
  auto cpu_impls =
      registry.get_implementations_by_device("m31_test", "invert", Device::CPU);
  ASSERT_EQ(cpu_impls.size(), 1);
  EXPECT_EQ(cpu_impls[0].metadata.device_preference, Device::CPU);

  auto metal_impls = registry.get_implementations_by_device(
      "m31_test", "invert", Device::GPU_METAL);
  ASSERT_EQ(metal_impls.size(), 1);
  EXPECT_EQ(metal_impls[0].metadata.device_preference, Device::GPU_METAL);

  auto cuda_impls = registry.get_implementations_by_device("m31_test", "invert",
                                                           Device::GPU_CUDA);
  ASSERT_EQ(cuda_impls.size(), 1);
  EXPECT_EQ(cuda_impls[0].metadata.device_preference, Device::GPU_CUDA);

  // 检索不存在的设备
  auto npu_impls = registry.get_implementations_by_device("m31_test", "invert",
                                                          Device::ASIC_NPU);
  EXPECT_TRUE(npu_impls.empty());
}

// 测试：按 Metadata 检索（cost_score）
TEST_F(OpRegistryM31Test, GetMetadataWithCostScore) {
  auto& registry = OpRegistry::instance();

  // 注册带有不同 cost_score 的实现
  OpMetadata meta;
  meta.device_preference = Device::CPU;
  meta.cost_score = 150;
  meta.tile_preference = TileSizePreference::MACRO;
  registry.register_impl("m31_test", "contrast", Device::CPU, dummy_cpu_op,
                         meta);

  auto impls = registry.get_implementations_by_device("m31_test", "contrast",
                                                      Device::CPU);
  ASSERT_EQ(impls.size(), 1);
  EXPECT_EQ(impls[0].metadata.cost_score, 150);
  EXPECT_EQ(impls[0].metadata.tile_preference, TileSizePreference::MACRO);
}

// 测试：选择最优实现（HP 模式：GPU 优先）
TEST_F(OpRegistryM31Test, SelectBestImplementationForHP) {
  auto& registry = OpRegistry::instance();

  // 注册 CPU 和 Metal 版本
  OpMetadata cpu_meta;
  cpu_meta.device_preference = Device::CPU;
  cpu_meta.cost_score = 100;
  registry.register_impl("m31_test", "sharpen", Device::CPU, dummy_cpu_op,
                         cpu_meta);

  OpMetadata metal_meta;
  metal_meta.device_preference = Device::GPU_METAL;
  metal_meta.cost_score = 50;
  registry.register_impl("m31_test", "sharpen", Device::GPU_METAL,
                         dummy_metal_op, metal_meta);

  // HP 模式下，当 GPU 可用时应选择 GPU
  std::vector<Device> available_devices = {Device::CPU, Device::GPU_METAL};
  auto best = registry.select_best_implementation(
      "m31_test", "sharpen", available_devices,
      ComputeIntent::GlobalHighPrecision);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.device_preference, Device::GPU_METAL);
}

// 测试：选择最优实现（HP 模式：仅 CPU 可用）
TEST_F(OpRegistryM31Test, SelectBestImplementationHPCpuOnly) {
  auto& registry = OpRegistry::instance();

  // 注册 CPU 和 Metal 版本
  OpMetadata cpu_meta;
  cpu_meta.device_preference = Device::CPU;
  cpu_meta.cost_score = 100;
  registry.register_impl("m31_test", "denoise", Device::CPU, dummy_cpu_op,
                         cpu_meta);

  OpMetadata metal_meta;
  metal_meta.device_preference = Device::GPU_METAL;
  metal_meta.cost_score = 50;
  registry.register_impl("m31_test", "denoise", Device::GPU_METAL,
                         dummy_metal_op, metal_meta);

  // 仅 CPU 可用时应选择 CPU
  std::vector<Device> available_devices = {Device::CPU};
  auto best = registry.select_best_implementation(
      "m31_test", "denoise", available_devices,
      ComputeIntent::GlobalHighPrecision);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.device_preference, Device::CPU);
}

// 测试：选择最优实现（RT 模式：Tiled CPU 优先）
TEST_F(OpRegistryM31Test, SelectBestImplementationForRT) {
  auto& registry = OpRegistry::instance();

  // 注册 Tiled CPU 和 Monolithic Metal 版本
  OpMetadata cpu_tiled_meta;
  cpu_tiled_meta.device_preference = Device::CPU;
  cpu_tiled_meta.cost_score = 80;
  cpu_tiled_meta.tile_preference = TileSizePreference::MICRO;
  registry.register_impl("m31_test", "levels", Device::CPU, dummy_tiled_cpu_op,
                         cpu_tiled_meta);

  OpMetadata metal_meta;
  metal_meta.device_preference = Device::GPU_METAL;
  metal_meta.cost_score = 50;
  registry.register_impl("m31_test", "levels", Device::GPU_METAL,
                         dummy_metal_op, metal_meta);

  // RT 模式下，Tiled CPU 应优先（低延迟）
  std::vector<Device> available_devices = {Device::CPU, Device::GPU_METAL};
  auto best = registry.select_best_implementation(
      "m31_test", "levels", available_devices, ComputeIntent::RealTimeUpdate);

  ASSERT_TRUE(best.has_value());
  // RT 模式优先选择 Tiled CPU
  EXPECT_EQ(best->metadata.device_preference, Device::CPU);
  EXPECT_TRUE(best->is_tiled());
}

// 测试：OpImplementation 辅助方法
TEST_F(OpRegistryM31Test, OpImplementationHelperMethods) {
  auto& registry = OpRegistry::instance();

  // 注册 Monolithic 实现
  OpMetadata mono_meta;
  mono_meta.device_preference = Device::CPU;
  registry.register_impl("m31_test", "mono_op", Device::CPU, dummy_cpu_op,
                         mono_meta);

  // 注册 Tiled 实现
  OpMetadata tiled_meta;
  tiled_meta.device_preference = Device::GPU_METAL;
  tiled_meta.tile_preference = TileSizePreference::MACRO;
  registry.register_impl("m31_test", "tiled_op", Device::GPU_METAL,
                         dummy_tiled_metal_op, tiled_meta);

  auto mono_impls = registry.get_implementations_by_device(
      "m31_test", "mono_op", Device::CPU);
  ASSERT_EQ(mono_impls.size(), 1);
  EXPECT_TRUE(mono_impls[0].is_monolithic());
  EXPECT_FALSE(mono_impls[0].is_tiled());

  auto tiled_impls = registry.get_implementations_by_device(
      "m31_test", "tiled_op", Device::GPU_METAL);
  ASSERT_EQ(tiled_impls.size(), 1);
  EXPECT_FALSE(tiled_impls[0].is_monolithic());
  EXPECT_TRUE(tiled_impls[0].is_tiled());
}

// 测试：不存在的算子返回空
TEST_F(OpRegistryM31Test, NonExistentOpReturnsEmpty) {
  auto& registry = OpRegistry::instance();

  auto impls = registry.get_all_implementations("nonexistent", "op");
  EXPECT_TRUE(impls.empty());

  auto by_device =
      registry.get_implementations_by_device("nonexistent", "op", Device::CPU);
  EXPECT_TRUE(by_device.empty());

  auto best = registry.select_best_implementation(
      "nonexistent", "op", {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  EXPECT_FALSE(best.has_value());
}

// 测试：同一设备上的多个实现按 cost_score 排序
TEST_F(OpRegistryM31Test, MultipleSameDeviceImplsSortedByCost) {
  auto& registry = OpRegistry::instance();

  // 同一设备上注册多个实现（不同 cost_score）
  OpMetadata meta1;
  meta1.device_preference = Device::CPU;
  meta1.cost_score = 200;  // 较高成本
  registry.register_impl("m31_test", "multi_cpu", Device::CPU, dummy_cpu_op,
                         meta1);

  OpMetadata meta2;
  meta2.device_preference = Device::CPU;
  meta2.cost_score = 50;  // 较低成本（更优）
  registry.register_impl("m31_test", "multi_cpu", Device::CPU, dummy_cpu_op,
                         meta2);

  // 选择最优时应返回 cost_score 较低的
  std::vector<Device> available_devices = {Device::CPU};
  auto best = registry.select_best_implementation(
      "m31_test", "multi_cpu", available_devices,
      ComputeIntent::GlobalHighPrecision);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.cost_score, 50);
}

TEST_F(OpRegistryM31Test, FilteredSelectionKeepsHpDevicePriority) {
  constexpr const char* kType = "m31_filter";
  constexpr const char* kSubtype = "hp_shape_filtered";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata gpu_monolithic_meta;
  gpu_monolithic_meta.device_preference = Device::GPU_METAL;
  gpu_monolithic_meta.cost_score = 1;
  registry.register_impl(kType, kSubtype, Device::GPU_METAL, dummy_metal_op,
                         gpu_monolithic_meta);

  OpMetadata cpu_tiled_meta;
  cpu_tiled_meta.device_preference = Device::CPU;
  cpu_tiled_meta.cost_score = 5;
  cpu_tiled_meta.tile_preference = TileSizePreference::MICRO;
  registry.register_impl(kType, kSubtype, Device::CPU, dummy_tiled_cpu_op,
                         cpu_tiled_meta);

  OpMetadata gpu_tiled_meta;
  gpu_tiled_meta.device_preference = Device::GPU_METAL;
  gpu_tiled_meta.cost_score = 100;
  gpu_tiled_meta.tile_preference = TileSizePreference::MICRO;
  registry.register_impl(kType, kSubtype, Device::GPU_METAL,
                         dummy_tiled_metal_op, gpu_tiled_meta);

  const std::vector<Device> available_devices = {Device::CPU,
                                                 Device::GPU_METAL};
  const auto best = registry.select_best_implementation(
      kType, kSubtype, available_devices, ComputeIntent::GlobalHighPrecision,
      [](const OpImplementation& impl) { return impl.is_tiled(); });

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.device_preference, Device::GPU_METAL);
  EXPECT_TRUE(best->is_tiled());
  EXPECT_EQ(best->metadata.cost_score, 100);

  registry.unregister_key(make_key(kType, kSubtype));
}

TEST_F(OpRegistryM31Test, FilteredSelectionReturnsNullWhenAllRejected) {
  constexpr const char* kType = "m31_filter";
  constexpr const char* kSubtype = "reject_all";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata cpu_meta;
  cpu_meta.device_preference = Device::CPU;
  cpu_meta.cost_score = 10;
  registry.register_impl(kType, kSubtype, Device::CPU, dummy_cpu_op, cpu_meta);

  const auto best = registry.select_best_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision,
      [](const OpImplementation&) { return false; });

  EXPECT_FALSE(best.has_value());

  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Allows a selection filter to inspect the same registry safely.
 * @throws Nothing when the copied candidate snapshot is filtered outside the
 * registry state lock.
 * @note The caller-provided filter runs outside the state lock so it cannot
 * extend the critical section or re-enter mutation during locked iteration.
 */
TEST_F(OpRegistryM31Test, CandidateFilterCanInspectRegistrySnapshot) {
  constexpr const char* kType = "m31_filter_inspection";
  constexpr const char* kSubtype = "cpu";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata metadata;
  metadata.cost_score = 7;
  registry.register_impl(kType, kSubtype, Device::CPU, dummy_cpu_op, metadata);

  const auto best = registry.select_best_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision,
      [&](const OpImplementation& candidate) {
        const auto observed = registry.get_metadata(kType, kSubtype);
        return observed.has_value() &&
               observed->cost_score == candidate.metadata.cost_score;
      });

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->metadata.cost_score, 7);
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Verifies unified selection preserves complete metadata and identity.
 *
 * @return Nothing; GoogleTest assertions report mismatched metadata.
 * @throws Registry allocation and callback-copy exceptions unchanged.
 * @note The selected value is one coherent CPU implementation snapshot; no
 * legacy metadata lookup participates after selection.
 */
TEST_F(OpRegistryM31Test, UnifiedSelectionCarriesSchedulingResourceMetadata) {
  constexpr const char* kType = "issue82_metadata";
  constexpr const char* kSubtype = "complete";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata metadata;
  metadata.reentrant = false;
  metadata.maximum_parallelism = 3U;
  metadata.retained_memory_bytes = 4096U;
  metadata.scratch_bytes = 8192U;
  metadata.exclusive_key = "issue82-shared-context";
  metadata.cost_score = 17;
  registry.register_impl(kType, kSubtype, Device::CPU, dummy_cpu_op, metadata);

  const auto selected = registry.select_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  EXPECT_NE(selected->implementation_identity, 0U);
  EXPECT_EQ(selected->metadata.device_preference, Device::CPU);
  EXPECT_FALSE(selected->metadata.reentrant);
  EXPECT_EQ(selected->metadata.maximum_parallelism, 3U);
  EXPECT_EQ(selected->metadata.retained_memory_bytes, 4096U);
  EXPECT_EQ(selected->metadata.scratch_bytes, 8192U);
  EXPECT_EQ(selected->metadata.exclusive_key, "issue82-shared-context");
  EXPECT_EQ(selected->metadata.cost_score, 17);
  EXPECT_FALSE(registry
                   .select_implementation(kType, kSubtype, {},
                                          ComputeIntent::GlobalHighPrecision)
                   .has_value());
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Verifies plugin-style scalar override and restoration change identity.
 *
 * @return Nothing; assertions report wrong override, metadata, or restoration.
 * @throws Registry capture, callback-copy, and restoration exceptions.
 * @note The registration capture models the plugin manager's transactional
 * override. Restoring the capture reinstates the exact core identity instead of
 * synthesizing a new revision.
 */
TEST_F(OpRegistryM31Test, PluginOverrideRestoresExactCoreIdentityAndMetadata) {
  constexpr const char* kType = "issue82_override";
  constexpr const char* kSubtype = "core_plugin";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata core_metadata;
  core_metadata.reentrant = false;
  core_metadata.maximum_parallelism = 1U;
  core_metadata.retained_memory_bytes = 101U;
  core_metadata.exclusive_key = "core-context";
  registry.register_op(kType, kSubtype, dummy_cpu_op, core_metadata);
  const auto core = registry.select_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(core.has_value());
  ASSERT_NE(core->implementation_identity, 0U);

  OpRegistry::RegistrationCapture plugin_capture;
  registry.capture_registration(
      [&registry] {
        OpMetadata plugin_metadata;
        plugin_metadata.reentrant = true;
        plugin_metadata.maximum_parallelism = 4U;
        plugin_metadata.retained_memory_bytes = 202U;
        plugin_metadata.scratch_bytes = 303U;
        plugin_metadata.exclusive_key = "plugin-context";
        registry.register_op(kType, kSubtype, dummy_metal_op, plugin_metadata);
      },
      plugin_capture);
  const auto plugin = registry.select_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(plugin.has_value());
  EXPECT_NE(plugin->implementation_identity, core->implementation_identity);
  EXPECT_TRUE(plugin->metadata.reentrant);
  EXPECT_EQ(plugin->metadata.maximum_parallelism, 4U);
  EXPECT_EQ(plugin->metadata.retained_memory_bytes, 202U);
  EXPECT_EQ(plugin->metadata.scratch_bytes, 303U);
  EXPECT_EQ(plugin->metadata.exclusive_key, "plugin-context");

  registry.restore_registration_capture(plugin_capture);
  const auto restored = registry.select_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->implementation_identity, core->implementation_identity);
  EXPECT_FALSE(restored->metadata.reentrant);
  EXPECT_EQ(restored->metadata.maximum_parallelism, 1U);
  EXPECT_EQ(restored->metadata.retained_memory_bytes, 101U);
  EXPECT_EQ(restored->metadata.scratch_bytes, 0U);
  EXPECT_EQ(restored->metadata.exclusive_key, "core-context");
  registry.unregister_key(make_key(kType, kSubtype));
}

/**
 * @brief Verifies malformed private metadata is rejected before mutation.
 *
 * @return Nothing; assertions report accepted oversized or embedded-NUL keys.
 * @throws Registry validation exceptions are consumed by GoogleTest.
 * @note The canonical key remains absent after both rejected registrations.
 */
TEST_F(OpRegistryM31Test, RejectsMalformedExclusiveKeysWithoutRegistration) {
  constexpr const char* kType = "issue82_invalid";
  constexpr const char* kSubtype = "exclusive_key";
  auto& registry = OpRegistry::instance();
  registry.unregister_key(make_key(kType, kSubtype));

  OpMetadata oversized;
  oversized.exclusive_key.assign(OpMetadata::kExclusiveKeyMaxBytes + 1U, 'x');
  EXPECT_THROW(registry.register_impl(kType, kSubtype, Device::CPU,
                                      dummy_cpu_op, oversized),
               std::invalid_argument);

  OpMetadata embedded_nul;
  embedded_nul.exclusive_key = std::string("invalid\0key", 11U);
  EXPECT_THROW(registry.register_impl(kType, kSubtype, Device::CPU,
                                      dummy_cpu_op, embedded_nul),
               std::invalid_argument);
  EXPECT_FALSE(registry
                   .select_implementation(kType, kSubtype, {Device::CPU},
                                          ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

// 测试：向后兼容性 - 传统 API 仍然可用
TEST_F(OpRegistryM31Test, BackwardCompatibilityWithLegacyAPI) {
  auto& registry = OpRegistry::instance();

  // 使用传统 API 注册
  OpMetadata meta;
  meta.device_preference = Device::CPU;
  registry.register_op("m31_compat", "legacy_op", dummy_cpu_op, meta);

  // 传统 API 仍然可以检索
  auto found = registry.find("m31_compat", "legacy_op");
  EXPECT_TRUE(found.has_value());

  auto metadata = registry.get_metadata("m31_compat", "legacy_op");
  EXPECT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->device_preference, Device::CPU);
}

}  // namespace
}  // namespace ps
