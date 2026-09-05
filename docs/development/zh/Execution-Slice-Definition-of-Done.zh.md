# Compiler 与 Execution Slice 完成定义

只有以下适用项全部成立，compiler/execution change 才算完成。

## 设计与 identity

- Change 指明受影响 stage：document、semantic IR、optimized IR、physical plan、
  runtime execution 或 public result。
- Stage identity 保持分离，并显式定义 canonical digest input。
- Operation semantic trait 覆盖每个新增 type/shape/Region/layout/backend influence。

## Correctness

- Duplicate/missing/cyclic graph error 与 malformed IR/plan input 在 publication 前
  失败。
- 按需具备 integer/byte-count overflow、bounds、alignment、pointer/count、shape、
  Region、layout、facet 与 buffer validation。
- Cancellation 与 stale completion 不能发布。
- Exception 被隔离，所有 resource/lease 精确释放一次。
- CPU 可用；可选 GPU selection 与允许的 fallback 已测试。

## 产品边界

- Kernel 不含 daemon Session/Job registry 或 result identity。
- Daemon consumer 只使用隔离安装的公开 package。
- 内部 IR、plugin path 与 native handle 不穿过 local IPC。
- 被删 service、durable-work、worker-process、policy DSO、plugin-security、
  durable-result 或 evidence product 不得通过 option 或 stub 返回。

## Verification 与文档

- Focused unit/integration/negative/concurrency test 通过。
- 受影响时 installed public header/export/consumer inventory 通过。
- 英文公开文档、中文镜像、GitHub Issue/Project 与 checked-in delivery snapshot
  一致。
- 私有 OpenSpec working note 不属于公开 completion gate。
- 记录实际 command 与 limitation；未运行 gate 不得宣称已运行。

## 决策任务与授权交付

仅包含决策的 Issue 应准备精确的拟议公共 API、备选方案、版本/所有权/错误/
身份影响、后续命名测试场景，以及维护者需决定的问题。审核文档与签名，不得
报告尚未实现的运行时测试已经通过，也不得启动依赖该决策的实现。决策在明确
接受前保持 Proposed，并须达到 Issue 指定交付位置后才能完成。本轮授权终点
为本地草案时，记录尚未完成的门槛。上述实现检查在相应代码存在时执行。状态
写入与交接遵循[任务协作](Task-Collaboration.zh.md)，私有 tracking 不替代公开交付。
