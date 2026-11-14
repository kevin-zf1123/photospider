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

Execution 头文件直接依赖 Plan 模型。实例化 `Compiler` 的调用方显式包含 `compiler/compiler.hpp` 或内核汇总头文件。默认 `Compiler` 构造器组装 frontend、optimizer 和 planner catalog；`CompilerComponents` 构造器接受显式组件选择。Plan 导入使用冻结的 operation catalog 与 `ProgramServices`，无需构造 compiler。

## 源码职责与应用

| 源码目录 | 职责 |
| --- | --- |
| `src/lib/core/`、`src/lib/data/` | Status、取消、资源计账、Tensor/Result 存储、视图与 Region 原语。Data 只包含 core/data 头文件。 |
| `sdk/model/` | Workflow document/context，以及物理 Task/Tile/Call/Block 模型与校验。 |
| `sdk/operation/` | 算子注册、C ABI 适配、静态 preparation、受检 program 构造与映射分派。 |
| `sdk/program/` | IR 绑定、Plan 链接/校验/裁剪、规范 identity 与 artifact codec。 |
| `sdk/execution/` | 面向 provider 的执行 API、Plan/data 服务、binding preflight 与宿主线程退休。 |
| `plugins/compiler/default/` | 编译阶段编排与 frontend/optimizer/planner 分派。 |
| `plugins/frontends/dag/`、`plugins/optimizers/deduplicate/`、`plugins/planners/static_tiles/` | DAG 前端、可选去重与静态 tile planner 的具体实现。 |
| `plugins/executors/physical/` | 物理调度、block memory、cache、CPU worker 与可选原生 GPU 执行。 |
| `plugins/mappers/geometry/` | 编译期几何求解器及注册/module 入口。 |
| `host/` | Catalog 组装、默认组件工厂、provider 加载、planner module bridge，以及宿主持有的 Metal callback。 |
| `apps/common/`、`apps/psc/`、`apps/psx/`、`apps/benchmark/` | CLI 公共 I/O、编译端 driver、执行端 driver 与可选 benchmark 客户端。 |
| `plugins/ops/common/domain/`、`plugins/ops/<NN-family>/` | 算子领域 SDK 与内置算子实现。 |
| `tests/fixtures/`、`tests/unit/kernel/`、`tests/integration/kernel/` | 合成 ABI producer 与持续维护的内核行为测试。 |
| `tests/package/`、`tests/tools/` | 包元数据契约与应用行为测试。 |

`sdk/program/` 中的 Plan 导入链接已保存的声明并校验物理表，不调用 frontend、optimizer、mapper 或 planner。执行 SDK 消费绑定后的 Plan 和显式选择的 provider。Physical provider 拥有调度与 GPU 实现依赖。`src/lib/core/`、`src/lib/data/` 不反向依赖 compiler、operation catalog 或 executor。

CLI 公共库不拥有 compiler session 或 execution context。`psc` 链接 compiler 和编译期 mapper 组件；`psx` 链接 execution 组件，可在没有 compiler 或 mapper catalog 时导入 `.psxp`。可执行入口分别派发到类型化 driver 库。CLI JSON 使用 [YYJSON.md](../../../third_party/YYJSON.md) 所述固定版本的私有 yyjson 源码，其声明不进入公共头文件。

完整阶段与生命周期契约见 [Compiler and Execution](../../kernel-architecture/zh/Compiler-and-Execution.zh.md)。文件及应用契约见 [Compiler Tools](../../development/zh/Compiler-Tools.zh.md) 和 [Compiler Tool Formats](../../development/zh/Compiler-Tool-Formats.zh.md)。

## Target 与依赖方向

`cmake/KernelComponents.cmake` 定义组件边界。下表中的组件名称使用 `Photospider::` 命名空间；可执行文件与私有 target 使用其显式名称。

| Target | 构建条件 | 职责 |
| --- | --- | --- |
| `data`、`model` | 始终构建 | 存储/资源原语与物理/workflow 模型。 |
| `plugin_host`、`operation_runtime`、`program` | 始终构建 | Provider 生命周期与加载、operation 契约、绑定后 Plan/artifact 服务。 |
| `operation_sdk` | 始终构建 | 仅含头文件的 C operation/provider ABI；纯 C producer 无需链接内核。 |
| `compiler`、`compiler_default` | `PHOTOSPIDER_BUILD_COMPILER=ON` | 编译编排；默认组装增加 DAG、deduplicate 与 static-tiles provider。 |
| `frontend_dag`、`optimizer_deduplicate`、`planner_static_tiles`、`mapper_geometry` | compiler 启用 | 可独立链接的编译期实现。 |
| `execution`、`executor_physical`、`execution_default` | `PHOTOSPIDER_BUILD_EXECUTOR=ON` | 面向 provider 的执行 SDK、physical 实现及两者的默认组装。 |
| `metal_host` | Apple 上启用 executor 和 Metal | 宿主持有的 Metal completion 与页释放 callback。 |
| `kernel`（`photospider`） | compiler 与 executor 均启用 | 组合 `compiler_default` 和 `execution_default` 的 interface 便利 target。 |
| `ops_sdk` | `PHOTOSPIDER_BUILD_OPS_SDK=ON` | 领域契约、不可变资源与数值 helper。 |
| `ops_runtime`、`ops_headers` | `PHOTOSPIDER_BUILD_OPS=ON` | 运行时注册/实现与内置算子公共头文件。 |
| `ops` | ops 和 compiler 均启用 | Runtime ops、workflow 编写 helper 与默认几何 mapper catalog。 |
| `benchmark_tools` | `PHOTOSPIDER_BUILD_BENCHMARK_TOOLS=ON` | 可选原始 benchmark 客户端。 |
| `psc`、`psx` | tools 与相应的 compiler/executor 选项启用 | 独立的编译端、执行端可执行文件。 |
| `result_files` | tools 与 executor 启用 | 安装的静态 Result 文件导出库。 |
| `photospider_cli_common`、`photospider_cli_json` | tools 启用 | 安装并导出的 CLI I/O 与 JSON 支持归档。 |
| `photospider_psc`、`photospider_psx`、测试 fixture | 仅构建 | 私有 driver 与定向行为测试。 |

Compiler、executor 与 tools 默认 ON，内置算子与 benchmark 工具默认 OFF。内置算子依赖 ops SDK。启用 ops 时，CLI environment 注册 `ops_runtime`。编译期 `ops` 便利库增加 `compiler_default` 和 `mapper_geometry`；仅执行所需的 `ops_runtime` 不引入这些依赖。

仅编译的构建关闭 `PHOTOSPIDER_BUILD_EXECUTOR`；仅执行的构建关闭 `PHOTOSPIDER_BUILD_COMPILER`。后者不生成同时使用 compiler/executor API 的 `test_cli_io`。动态 provider 测试显式链接 `compiler_default` 和 `execution`，并把 provider module 作为单独的构建依赖，保证宿主不会预先加载 physical executor。

共享 `ops` 保留包 VERSION/SOVERSION 与隐藏实现符号的属性，静态版本保留位置无关代码。内核组件不依赖 CLI driver、内置算子或 daemon 源码。内核开发阶段继续排除安装消费测试；保留的包测试检查包元数据。原生模块属于可信的进程内扩展。

## 命名与文档

类型使用 `PascalCase`；函数、字段、文件与内部 target 使用 `snake_case`。Compiler 扩展具有独立于实现文件路径的显式 catalog 名称。重命名同步更新声明、定义、include、测试、CMake 以及受影响的中英文文档。

公共 API 文档说明适用的参数、错误、所有权、生命周期、线程安全以及缓存或调度约束。内部注释解释不可变阶段发布、资源保留和回收顺序等约束。产品文档描述当前行为；历史恢复记录与维护中的契约分开保存。
