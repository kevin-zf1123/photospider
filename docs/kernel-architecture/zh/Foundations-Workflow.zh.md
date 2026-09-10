# 可组合 foundations workflow

本地 0.7 已实现 [ADR 0020](../../adr/0020-composable-operation-foundations.md) 的
G1/G2/G3/G5 数值、语义、输出、标量契约及可复用算子子集。交付目标为 ops，已审计
main@fba06270 仍为 package 0.6.0；本实现状态不表示 PR CI、外部审查或合并已完成。
Daemon 既有 0.6 consumer 尚未迁移。[英文指南](../Foundations-Workflow.md)为权威说明。

[自包含示例](../../../examples/foundations_workflow/README.zh.md)只用安装公开 API。
复制目录到仓库外，安装静态或共享内核，CMAKE_PREFIX_PATH 指向安装目录，运行
photospider_foundations_workflow --scenario all。README 给出精确命令、输入输出
shape、参数、Region、预期结果和修改组合方法。

| 组合 | 算子契约 |
| --- | --- |
| Cast/range、signed 算术/归约 | [数值算子](Numeric-Operations.zh.md) |
| Extract/process/merge、alpha、参考白转换 | [通道与颜色](Channel-and-Color-Operations.zh.md) |
| Expression、LUT、compile-once 动态 gain | [Expression/LUT](Expression-and-LUT-Operations.zh.md)、[图像端口](Image-Operations.zh.md) |
| Binary mask、稳定 labels、固定容量属性 | [组件算子](Component-Operations.zh.md) |
| Image-v2 快照、frozen input、有界结果缓存 | [缓存模型](Cache-Model.zh.md) |

六个独立场景及 all 已注册 CTest。隔离 installed consumer 使用导出 target 构建同一
示例，按 producer 传入 sanitizer 选项。Focused 跨功能命令：

```sh
cmake --build build/issue257-static --target test_numeric_operations test_color_operations test_expression_operations test_component_operations test_computed_scalar -j 8
ctest --test-dir build/issue257-static -R '^test_(foundations_.*|numeric_operations|color_operations|expression_operations|component_operations|computed_scalar|installed_consumer)$' --output-on-failure
```

共享消费替换为既有 shared build。场景源码维护独立样本/错误 oracle，更深边界、strided
view、浮点环境、取消和缓存竞态由各算子指南链接的 focused integration tests 验证。
八个迁移图像算子的 CPU/C/Metal 独立验收见 [S4](S4-Workflow.zh.md)，CPU 示例成功
不表示发生 GPU dispatch。

G4 逐端口空间依赖、G6 宿主资产、daemon 迁移、完整路径、FFT、ICC/OCIO 与其余 Proposed
目录均在范围外。新增全局/shape-changing 操作 Whole，每节点单输出。
