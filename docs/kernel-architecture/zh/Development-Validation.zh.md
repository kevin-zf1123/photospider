# 开发与验证说明

本文档记录影响内核可信度的仓库级验证预期。

## 主线 macOS 架构

主线 macOS 开发目标是 Apple Silicon `arm64`。

项目不打算保留主线 `x86_64` macOS 支持。如果未来用户需要 `x86_64`，可以通过分支、fork 或专门兼容性工作处理。

在 Apple Silicon 上，编译器目标、终端架构和依赖架构应全部同意为 `arm64`。`x86_64` 构建与 `arm64` Homebrew 库之间的架构不匹配不是受支持的主线设置。

## 构建配置方向

开发者设置应明确架构选择。CMake presets 或 bootstrap 说明应默认 macOS 为 `arm64`。

仓库还应记录或提供：

- `compile_commands.json` 生成
- lint 和格式化命令
- 预期本地验证命令集

根 CMake 配置会导出 `compile_commands.json`，并在没有提供 `CMAKE_OSX_ARCHITECTURES` 值时默认主线 macOS 构建为 `arm64`。

## CTest 注册

所有预期 GoogleTest 二进制都应注册到 CTest。这包括当前可能低置信度的里程碑和传播测试。

低置信度测试仍应在验证中可见，而不是被静默排除。如果测试不足以可靠地作为开发门禁，应明确记录该状态，并创建后续工作升级或替换它。

里程碑和传播测试已注册到 CTest，因此它们可见；但在后续 pass 将它们重写为更窄、更清晰 fixture 和断言的回归测试前，它们仍是低置信度遗留测试。

## 已知测试质量注意事项

一些里程碑和传播测试最初是开发检查，而不是精修过的回归测试。它们应被注册以保持可见，然后在后续升级为更清晰、更高置信度的测试。

## 重构边界

以下是已识别的后续重构，不属于当前 kernel-contract 清理：

- 在 frontend 展示契约定义后，添加更丰富的 dirty snapshot 可视化 API。
- 决定 global HP dirty ROI 是否应在所有入口路径中使用优化后的局部 HP update planning。

`ComputeService` 拆分现在已有专门的 `split-compute-service` OpenSpec change，
并在维护文档 `Compute-Service-Split.md` 中记录计划。第一轮拆分已经通过
`src/kernel/services/compute-service/` 下的内部模块实现。边界覆盖位于
`tests/test_compute_service_split.cpp`，并保留 `test_kernel_contracts`、
`test_propagation_contracts`、`test_scheduler`、`test_milestone34` 和
`test_gpu_pipeline_scheduler` 的回归覆盖。

`GraphTraversalService` 拓扑/ROI 拆分已经落地。边界覆盖位于
`tests/test_graph_topology_boundaries.cpp`、`tests/test_propagation_contracts.cpp`，
以及 `tests/results/split-graph-traversal-service/` 下的可复现证据。
