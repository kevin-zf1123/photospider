/**
 * @file test_op_registry_m31.cpp
 * @brief M3.1 里程碑测试：OpRegistry 多设备实现支持
 *
 * 验收标准：
 * - 单元测试能注册同一算子的 CPU 和 Metal 版本
 * - 能按 Metadata 检索
 */

#include <gtest/gtest.h>

#include <vector>

#include "ps_types.hpp"

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
  EXPECT_EQ(cpu_impls[0]->metadata.device_preference, Device::CPU);

  auto metal_impls = registry.get_implementations_by_device(
      "m31_test", "invert", Device::GPU_METAL);
  ASSERT_EQ(metal_impls.size(), 1);
  EXPECT_EQ(metal_impls[0]->metadata.device_preference, Device::GPU_METAL);

  auto cuda_impls = registry.get_implementations_by_device("m31_test", "invert",
                                                           Device::GPU_CUDA);
  ASSERT_EQ(cuda_impls.size(), 1);
  EXPECT_EQ(cuda_impls[0]->metadata.device_preference, Device::GPU_CUDA);

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
  EXPECT_EQ(impls[0]->metadata.cost_score, 150);
  EXPECT_EQ(impls[0]->metadata.tile_preference, TileSizePreference::MACRO);
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

  ASSERT_NE(best, nullptr);
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

  ASSERT_NE(best, nullptr);
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

  ASSERT_NE(best, nullptr);
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
  EXPECT_TRUE(mono_impls[0]->is_monolithic());
  EXPECT_FALSE(mono_impls[0]->is_tiled());

  auto tiled_impls = registry.get_implementations_by_device(
      "m31_test", "tiled_op", Device::GPU_METAL);
  ASSERT_EQ(tiled_impls.size(), 1);
  EXPECT_FALSE(tiled_impls[0]->is_monolithic());
  EXPECT_TRUE(tiled_impls[0]->is_tiled());
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
  EXPECT_EQ(best, nullptr);
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

  ASSERT_NE(best, nullptr);
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
  const OpImplementation* best = registry.select_best_implementation(
      kType, kSubtype, available_devices, ComputeIntent::GlobalHighPrecision,
      [](const OpImplementation& impl) { return impl.is_tiled(); });

  ASSERT_NE(best, nullptr);
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

  const OpImplementation* best = registry.select_best_implementation(
      kType, kSubtype, {Device::CPU}, ComputeIntent::GlobalHighPrecision,
      [](const OpImplementation&) { return false; });

  EXPECT_EQ(best, nullptr);

  registry.unregister_key(make_key(kType, kSubtype));
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
