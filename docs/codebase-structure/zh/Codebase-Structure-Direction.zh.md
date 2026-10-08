# Codebase 结构方向

ADR 0015 定义破坏性 repository boundary。Kernel tree 包含可嵌入
compiler/executor 与可信 operation/provider extension；local daemon orchestration
只位于 `photospider-daemon`。

## 公开布局

只有 `include/photospider/**` 可安装：

```text
include/photospider/
  compiler/      WorkflowDocument、typed IR、plan、typed identity、compiler
  execution/     context、cancellation、result、raw diagnostic
  data/          Value、Region、explicit layout 与 immutable bytes
  plugin/        operation 与 data-provider ABI/registry
  core/          status 与 symbol export
  photospider.hpp 完整 convenience include
```

Public header 绝不 include `src/lib`、暴露 private compiler/planner node、命名 native
device object 或要求 sibling checkout。不存在 public policy、server、daemon、worker、
evidence 或 durable-result header。

## 私有布局

```text
src/lib/
  compiler/
  data/
  graph/
  execution/
  plugin/

tests/consumer/
tests/fixtures/
tests/unit/
tests/integration/
```

Private source home 按职责划分。Active tree 没有 `src/lib/server`、
`src/lib/policy`、process-isolation subtree、`plugins/policies`、worker application 或
execution-profile benchmark family。

## Target 形态

| Target | 安装 | 角色 |
| --- | --- | --- |
| `photospider` / `Photospider::kernel` | 是 | 单一 public compiler/executor 与 ABI runtime |
| `Photospider::operation_sdk` | 是 | 仅 header 的可信 operation DSO ABI |
| `Photospider::data_provider_sdk` | 是 | 仅 header 的 data-definition/provider DSO ABI |
| operation/provider fixture module | 否 | 仅测试 ABI validation 与 lifecycle |
| test executable | 否 | 维护 unit/integration/package behavior |

被删产品没有 option、default-OFF target、component、export、install rule、preset 或
compatibility alias。

## 依赖方向

```text
public values and traits
  <- compiler <- optimizer <- planner <- executor
  <- operation/provider host adapters

photospider-daemon
  -> installed Photospider::kernel
```

Kernel 绝不依赖 daemon source/package target。Daemon test 把 kernel 安装到 fresh
prefix，并且只使用 public package export。

## 命名与文档

Type 使用 `PascalCase`；file、function、field、directory 与 internal target 使用
`snake_case`。完整 rename 会更新 declaration、definition、include、test、CMake、
公开 document、mirror 与 tracked Issue，且不留 alias。私有 OpenSpec working note
对 rename 没有 authority。

公共 API 文档说明适用的参数约束、返回值与错误行为、所有权、生命周期、线程安全，
以及影响契约的缓存或调度约束。内部注释解释不明显的状态转移、预算归属和回收顺序；
普通 helper 无需逐一编写完整 Doxygen。
