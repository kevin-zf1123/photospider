# Codebase 结构方向

ADR 0015 定义破坏性的仓库边界。内核仓库包含可嵌入的 compiler/executor 以及可信的 operation/provider 扩展；本地 daemon 编排只位于 `photospider-daemon`。

## 公开布局

可安装的 include 根目录有两个，都安装到 `photospider/` 前缀下：

```text
include/photospider/
  compiler/      WorkflowDocument、typed IR、plan、typed identity、compiler
  execution/     context、cancellation、result、raw diagnostic
  data/          Value、Region、显式 layout 与不可变字节
  plugin/        operation 与 data-provider ABI/registry
  core/          status、resources、cancellation 与符号导出
  benchmark/     原始 compile/plan/execute benchmark runner
  photospider.hpp 内核汇总头文件

plugins/ops/include/photospider/
  ops/format/    format/color 的 workflow 编写辅助
  ops/numeric/   numeric 的 workflow 编写辅助
  ops/           FFT、component 与 statistics 算子工厂
  ops.hpp        内核汇总头文件，加上 format、FFT、component 与 statistics 辅助
```

`include/photospider/` 只包含内核契约。编码内置算子名称的 workflow 编写辅助和具体算子的工厂放在 `plugins/ops/include/photospider/ops/`。`photospider.hpp` 不包含 `ops/` 下的任何头文件。

公开头文件绝不 include `src/lib`，不暴露私有 compiler/planner 节点，不命名 native device 对象，也不要求 sibling checkout。不存在公开的 policy、server、daemon、worker、evidence 或 durable-result 头文件。

## 私有布局

```text
src/lib/
  core/          共享原语：带溢出检查的算术、Status 工厂、
                 精确二进制求和、数值位辅助、资源状态
  data/
  compiler/
  graph/
  execution/
  plugin/
  benchmark/

plugins/ops/<NN-family>/   内置算子实现

tests/consumer/
tests/fixtures/
tests/unit/
tests/integration/
```

私有源码按职责划分位置。`src/lib/core/` 存放不依赖其他内核模块的原语；`checked_math.hpp`、`status_helpers.hpp`、`exact_binary_sum.hpp` 和 `numeric_bits.hpp` 是共享实现，其他模块不再定义本地副本。当前源码树中没有 `src/lib/server`、`src/lib/policy`、进程隔离子树、`plugins/policies`、worker 应用或 execution-profile benchmark 族。

## Target 形态

| Target | 安装 | 角色 |
| --- | --- | --- |
| `photospider` / `Photospider::kernel` | 是 | 唯一的公开 compiler/executor 与 ABI runtime；同时包含内置算子的实现 |
| `Photospider::ops_headers` | 是 | 只含头文件，提供 `photospider/ops/` 的 include 根；使用方与 `Photospider::kernel` 一起链接 |
| `Photospider::operation_sdk` | 是 | 只含头文件的可信 operation DSO ABI |
| `Photospider::data_provider_sdk` | 是 | 只含头文件的 data-definition/provider DSO ABI |
| operation/provider fixture 模块 | 否 | 仅用于测试 ABI 校验与生命周期 |
| 测试可执行文件 | 否 | 维护 unit/integration/package 行为 |

`plugins/ops/` 下的内置算子源码编译进内核库。它们 include `src/lib/core/` 和 `src/lib/data/` 的私有头文件，因此仓库中没有单独构建的内置算子库。

已删除的产品不保留 option、default-OFF target、component、export、install 规则、preset 或兼容别名。

## 依赖方向

```text
core <- data <- compiler <- optimizer <- planner <- executor
                                     <- operation/provider host adapters

plugins/ops (内置算子) -> 公开内核头文件 + 私有 core/data 头文件

photospider-daemon
  -> installed Photospider::kernel
```

`src/lib/data/` 和 `include/photospider/data/` 中的文件只 include `core` 与 `data` 头文件，从不 include execution、plugin、compiler 或算子族头文件。

内核绝不依赖 daemon 的源码或 package target。Daemon 测试把内核安装到新的 prefix，并且只使用公开的 package export。

## 命名与文档

类型使用 `PascalCase`；文件、函数、字段、目录与内部 target 使用 `snake_case`。完整的重命名会同步更新声明、定义、include、测试、CMake、公开文档、镜像与被跟踪的 Issue，且不保留别名。私有 OpenSpec 工作笔记对重命名没有约束力。

公共 API 文档说明适用的参数约束、返回值与错误行为、所有权、生命周期、线程安全，以及影响契约的缓存或调度约束。内部注释解释不明显的状态转移、预算归属和回收顺序；普通 helper 无需逐一编写完整 Doxygen。
