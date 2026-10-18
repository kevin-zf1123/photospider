# Codebase 结构方向

ADR 0015 定义仓库边界。内核拥有可嵌入的 compiler/executor 和可信的进程内 operation 扩展。`psc` 与 `psx` 应用负责编译和执行本地文件；daemon 编排属于 `photospider-daemon`。

## 公开布局

公开 include 树安装到 `photospider/` 前缀下。可选库分别控制自身头文件的安装。

```text
include/photospider/
  compiler/       WorkflowDocument、IR、Plan、编译阶段与 artifact codec
  execution/      execution context、bindings、cancellation 与 diagnostics
  data/           Result、Tensor、Region、layout 与不可变资源
  plugin/         operation ABI、registry 与可信宿主服务
  core/           status、resources、cancellation 与符号导出
  ops/            可选的算子领域 SDK
  benchmark/      可选的 benchmark 客户端 API
  photospider.hpp 可嵌入内核的汇总头文件

plugins/ops/include/photospider/
  ops/            内置算子工厂与 workflow 编写辅助
  ops.hpp         内核汇总头文件及内置算子辅助头
```

内核安装规则排除 `ops/` 与 `benchmark/`；`cmake/OpsSdk.cmake`、`cmake/BundledOperations.cmake` 和 `cmake/BenchmarkTools.cmake` 分别拥有这些可选导出。`photospider.hpp` 只包含内核 API。领域配置与具体算子名称属于 ops SDK 或内置算子编写辅助头。

公共头文件使用公开的内核类型，原生实现对象保留在私有实现中。消费方无需访问 `src/lib` 头文件或相邻仓库。

## Compiler 模型与阶段

| 公开头文件 | 职责 |
| --- | --- |
| `compiler/workflow_document.hpp` | DAG 声明、`GraphContext` 与一致快照。 |
| `compiler/ir.hpp` | SSA/CFG 数据、依赖路径、`ValidatedIR`、内容 identity 与 provenance。 |
| `compiler/plan.hpp` | 规划选项、固定指令、grants、`ExecutionPlan` 与物理 identity。 |
| `compiler/compilation.hpp` | 保留的 IR revision、可选 Plan 与阶段计时。 |
| `compiler/frontend.hpp` | 冻结的 frontend catalog 与类型化 DAG lowering callback。 |
| `compiler/optimizer.hpp` | 冻结的 optimizer catalog、参数 schema 与独立变换。 |
| `compiler/planner.hpp` | 冻结的 planner catalog 与 callback 局部的 `PlanBuilder`。 |
| `compiler/compiler.hpp` | 基于冻结 registry 的阶段编排。 |
| `compiler/artifact.hpp` | IR/Plan 文本导入、导出、检查与文件辅助。 |

Execution 头文件直接依赖 Plan 模型。实例化 `Compiler` 的调用方显式包含 `compiler/compiler.hpp` 或内核汇总头文件。Compiler 构造与 artifact 导入只初始化所请求阶段需要的 catalog。

| 私有源码 | 职责 |
| --- | --- |
| `src/lib/compiler/compiler.cpp` | Registry 所有权与 analyze/可选 optimize/plan 编排。 |
| `frontend.cpp`、`frontend_registry.cpp`、`frontend/DAG/dag.cpp` | Frontend 派发、catalog 与 DAG 依赖路径 lowering。 |
| `ir_validation.cpp`、`dependency_paths.hpp` | 通用 IR 校验、operation 绑定与私有路径 lowering。 |
| `optimizer.cpp`、`optimizer_registry.cpp`、`optimizers/deduplicate.cpp` | 变换调用、输出校验、catalog 与可选去重。 |
| `planner.cpp`、`planner_registry.cpp`、`planners/static_tiles.cpp` | 后端派发、catalog 与静态指令生成。 |
| `plan_linking.cpp`、`plan_validation.cpp`、`plan_selection.cpp` | 声明与 placement 链接、保存指令的验证以及 coverage 缩小。 |
| `static_mapping.cpp`、`identity.cpp` | 静态依赖辅助与规范内容/cache identity。 |
| `artifact.cpp`、`artifact_text.cpp` | Artifact 链接与有界文本语法。 |

后七行中的源码名称相对于 `src/lib/compiler/`。Plan 导入链接已保存的声明并验证已保存代码，过程中不调用 optimizer、mapper 或 planner。完整的阶段与生命周期契约见 [Compiler and Execution](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)。

## 私有布局与应用

```text
src/lib/
  core/          带溢出检查的算术、status 工厂与资源原语
  data/          Tensor/Result 存储、视图、region 与预算
  compiler/      阶段实现与 artifact codec
  graph/         workflow document/context 实现
  execution/     Plan 解释器、调度、发布与缓存
  plugin/        registry、DSO 加载与 C ABI 宿主适配

src/tools/
  common/        CLI 解析、诊断、信号、JSON 与文件/manifest I/O
  psc/           compiler driver、action graph、IR session 与 DAG JSON 输入
  psx/           executor driver、运行时绑定与 Result 导出
  benchmark/     可选的原始 benchmark 客户端实现

plugins/ops/common/domain/ 算子领域 SDK 实现
plugins/ops/<NN-family>/   内置算子实现与辅助

tests/fixtures/            合成算子与 ABI 模块
tests/unit/kernel/        内核行为测试
tests/integration/kernel/ 内核执行与 ABI 行为测试
tests/package/            包元数据契约
tests/tools/              应用与可选 benchmark 行为测试
```

`src/lib/core/` 包含不依赖其他内核模块的原语。Data 实现只包含 core/data 头文件。Compiler 阶段使用 operation metadata 与静态 preparation；execution 消费 Plan 并调用 operation program。CLI 公共库不拥有 compiler session 或 execution context。`psc` 与 `psx` 分别拥有类型化 invocation 和 driver 库；可执行入口仅派发到对应 driver。

CLI JSON 使用 [YYJSON.md](../../../third_party/YYJSON.md) 所述固定版本的私有 yyjson 源码，其声明不进入安装后的内核头文件。文件格式与应用契约见 [Compiler Tools](../../development/zh/Compiler-Tools.zh.md) 和 [Compiler Tool Formats](../../development/zh/Compiler-Tool-Formats.zh.md)。

## Target 与依赖方向

| Target | 安装条件 | 职责 |
| --- | --- | --- |
| `photospider` / `Photospider::kernel` | 始终安装 | 可嵌入的 compiler/executor 与 operation ABI 宿主。 |
| `Photospider::operation_sdk` | 始终安装 | 仅含头文件的可信 operation DSO ABI。 |
| `Photospider::ops_sdk` | `PHOTOSPIDER_BUILD_OPS_SDK=ON` | 基于公开内核 API 的领域契约、资源与数值辅助。 |
| `Photospider::ops`、`Photospider::ops_headers` | `PHOTOSPIDER_BUILD_OPS=ON` | 独立内置算子实现库与编写辅助 include 根目录。 |
| `Photospider::benchmark_tools` | `PHOTOSPIDER_BUILD_BENCHMARK_TOOLS=ON` | 基于公开内核 API 的原始 benchmark 客户端。 |
| `psc`、`psx` | `PHOTOSPIDER_BUILD_TOOLS=ON` | 编译器与执行器可执行文件。 |
| `photospider_cli_json`、`photospider_cli_common`、`photospider_psc`、`photospider_psx` | 仅构建 | 私有 CLI 支持库与 driver 库。 |
| 测试可执行文件与 fixture 模块 | 仅构建 | 维护程序行为与包契约。 |

工具默认 ON，内置算子与 benchmark 工具默认 OFF。内置算子依赖 ops SDK。启用 ops 还会将其注册入口链接到 CLI environment；仅包含内核的 CLI 接受显式加载的 operation library。

```text
psc driver -> compiler/artifact API -> operation metadata 与 preparation
psx driver -> artifact/execution API -> operation runtime program
两个 driver -> CLI 公共 I/O -> 私有 yyjson

内置 ops -> ops SDK -> 公开内核 API
benchmark 客户端 -> 公开内核 API
photospider-daemon -> installed Photospider::kernel
```

内核不依赖工具 driver、内置算子或 daemon 源码。内核开发阶段排除安装消费测试；保留的包测试检查包元数据。原生 operation 加载属于可信的进程内边界。

## 命名与文档

类型使用 `PascalCase`；函数、字段、文件与内部 target 使用 `snake_case`。Compiler 扩展具有独立于实现文件路径的显式 catalog 名称。重命名同步更新声明、定义、include、测试、CMake 以及受影响的中英文文档。

公共 API 文档说明适用的参数、错误、所有权、生命周期、线程安全以及缓存或调度约束。内部注释解释不可变阶段发布、资源保留和回收顺序等约束。产品文档描述当前行为；历史恢复记录与维护中的契约分开保存。
