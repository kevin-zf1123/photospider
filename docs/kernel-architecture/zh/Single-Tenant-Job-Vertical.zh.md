# 单租户 Job 纵向路径

本文定义当前源码私有的 Issue #98 Job 行为，是 `src/lib/server/` 当前事实的权威来源。
ADR 0011 说明长期安全域决策，服务端 roadmap 说明后续交付切片。当前模块是一条可执行的
单进程纵向路径，不是网络或多租户服务端。

## 术语与当前剖面

当前剖面由 `jobspec-v1`、`embedded-cpu-v1` 执行和 `process-lifetime` 工件
durability 组成。它支持一个配置的 `TenantId`、任意数量的进程生命周期 Job、每个 Job
一个 attempt、每个 attempt 一个新的进程内 worker 对象与 Embedded Host、一个目标节点和
一个必需 CPU 图像输出槽。

下列身份域是相互不同的强 C++ 值类型：

| 身份 | 当前含义 | 明确不表示 |
| --- | --- | --- |
| `TenantId` | 一个 `SingleTenantJobService` 配置的唯一 tenant | 已认证 principal 或多租户授权 |
| `JobId` | 进程生命周期内一个已接受 immutable Job | 本地 IPC compute request 或 Graph session |
| `JobSpecDigest` | 精确规范 `jobspec-v1` 字节的 SHA-256 | 单独构成 Job 权威 |
| `JobAttemptId` | 本切片唯一的 current attempt | retry generation；当前没有 retry |
| `WorkerInstanceId` + `WorkerLeaseGeneration` | 精确的新 worker 对象及其唯一 assignment | OS process identity、PID 或 supervisor lease |
| `GraphArtifactId` | 由可信 resolver 解释的 immutable graph material key | Host path 或本地 Graph session |
| `OutputSlotId` | 必需图像输出声明 | runtime node/output pointer |
| `ArtifactId` | 进程生命周期 store 中一条 immutable artifact record | content digest、path、`OutputArtifactId` 或 buffer handle |
| `OutputCommitId` | 一次精确 artifact commit event | idempotency key 或 durable transaction id |
| `ArtifactContentDigest` | 精确 tight payload 字节的 SHA-256 | Artifact 或 commit identity |

所有生成的 Job、attempt、worker、artifact 和 commit id 在当前进程内不复用。它们不持久化、
不全局分配，重启后也不能恢复。

## Immutable JobSpec

`JobSpec` 是经过校验、只提供 getter 且没有 mutation surface 的类。构造器只接受：

- 一个有界 opaque `GraphArtifactId`；
- 一个非负目标节点整数；
- 一个有界 opaque 必需 `OutputSlotId`；
- 一个正数 maximum-parallelism 上限；
- 封闭的 `embedded-cpu-v1` profile；
- 请求的 `process-lifetime` durability。

规范字节以 `jobspec-v1` 开头，以十进制长度 frame 编码全部六个字段。target-node 与
maximum-parallelism 两个整数先采用规范十进制文本，再与其余四个字段完全相同地进行
framing。构造器记录这些精确字节的 SHA-256。控制面在接受前重新校验该值，以
`shared_ptr<const JobSpec>` 保留它，并在 Job 与 assignment state 中记录摘要。worker 在解析图
之前再次进行字段、规范字节和摘要校验。

JobSpec 不包含 Host path、`GraphLoadRequest`、本地 `GraphSessionId`、文件描述符、指针、
native/runtime handle、mutable store location、bearer credential、本地 IPC id、plugin DSO、
checkpoint、retry、quota 或 retention policy 字段。JobSpec 外的可信
`GraphArtifactResolver` 把图身份映射为当前 adapter 使用的本地 `GraphLoadRequest` path
字段。这些路径绝不进入规范 Job 字节、attempt report 或 artifact receipt。

## 可观察 Job 行为

`SingleTenantJobService` 是已接受 Job 真相的唯一 owner。`submit()` 校验并冻结 JobSpec，
生成 `JobId`、`JobAttemptId`、`WorkerInstanceId` 和 generation 为一的 lease，记录完整
assignment，然后启动一个可 join 的 worker thread。`query()` 返回复制的 snapshot；
`wait_for()` 只限制 observer 等待，不设置执行 deadline。

当前状态机是：

```text
Queued -> Running ---------------------> Succeeded
   |         |                              ^
   +---------+-> Cancelling -> Cancelled    |
             |                              |
             +---------------------------> Failed
```

`Succeeded`、`Failed` 和 `Cancelled` 是终态。没有 retry 或 attempt replacement。worker
只返回 immutable `JobAttemptReport`：完整 assignment tuple、worker-local outcome、
settlement fact、typed failure、diagnostic 和可选 candidate `ImageBuffer`。它不能修改 Job
snapshot 或提交 artifact。

控制面通过精确 worker thread 保留的 assignment 查找 Job，随后校验报告完整的 tenant/Job/
spec-digest/attempt/worker/lease tuple。不匹配、空、过期或 malformed 报告使当前 Job fail
closed，不附加工件。某个身份域相等或内容相等不能修复另一处不匹配。由于被拒绝的报告
不受信任，其 `settled` 字段也不能为保留的 current attempt 建立 settlement 事实。

Job 成功要求在当前 control mutex 下同时满足：

1. 报告匹配精确 current assignment；
2. attempt 报告 `Succeeded` 且 `settled=true`；
3. 报告携带一个有效非空 CPU image 且没有 failure fact；
4. 独立工件权威为声明槽提交该 image；
5. 返回回执匹配完整 assignment、slot 和请求的 process-lifetime durability。

因此 Host/Run 成功本身不表示 Job 成功。Artifact commit、Job terminal publication、
cancellation intent 和 caller observation 仍是不同事实。

## 取消与完成顺序

`cancel()` 记录一次单调 control-plane intent。Job 不存在、已终态或重复请求时返回 false。
接受活跃请求会把可观察状态改为 `Cancelling`；它不会 detach worker，也不声称 execution
deadline。

public Host 当前没有主动 compute-cancellation 操作。Embedded Host worker 在 graph
resolution 前、Host construction/load/compute 前以及 compute 后观察取消。Host compute
一旦进入 provider，取消可能无限期等待该调用返回。随后 worker 关闭已加载图并销毁 Host，
之后才报告。

取消与 artifact commit 在 Job mutex 下线性化：

- 若取消先发生，之后的 image 被丢弃，不提交 artifact，只有 `settled=true` 后才出现
  `Cancelled`；
- 若成功 commit 与 terminal publication 先发生，之后 cancel 返回 false，不能改写回执或
  `Succeeded` 状态。

service 析构会把活跃 Job 标为 cancelling 并 join 每个 worker。这是有序 ownership cleanup，
不是有界强制终止。WorkerManager、heartbeat、OS-process kill/reap、crash/hang/OOM containment
和 retry 属于 Issue #100 及后续工作。

## Embedded Host worker 路径

`EmbeddedHostJobWorker` 对一个 assignment 执行下列阶段：

1. 校验完整 assignment、immutable JobSpec 和精确摘要；
2. 观察取消；
3. 让可信 resolver 在 JobSpec 外解析图材料；
4. 创建新的 Embedded Host 并 seed 仓库 built-in operation；
5. 加载 attempt-local Graph session，其名称绝不成为服务端权威；
6. 用 `fp32`、force-recache、禁用 disk-cache、不保存 cache、quiet output 和 JobSpec
   maximum-parallelism 上限计算声明节点；
7. 校验非空 CPU `ImageBuffer` candidate；
8. 再次观察取消、关闭精确 Graph、销毁 Host ownership，随后才报告 settlement。

resolution、Host setup、load、compute、output validation 和 settlement 各有不同的
`JobAttemptFailure` 值。Graph resolution 失败不构造 Host。Graph close 失败报告
`settled=false`，不能成功。worker 从不获得 artifact-store mutation authority。若 factory
返回 null，或标准/非标准异常逃逸出 worker 创建或执行，control plane 没有 settlement
证据；它发布 `settled=false` 且没有回执的 current failed attempt。即便已经接受取消，也
不能把该未 settle 失败改写为 `Cancelled`。

## 进程生命周期工件权威

`ProcessLifetimeArtifactStore` 是独立于 Job state、本地 IPC `OutputStore` 和 benchmark
`B1OutputStore` 的对象。Commit 校验完整 assignment 与 output slot，调用 public
`ImageBuffer` validator，要求非空 CPU payload，把 active byte 逐行复制到 tight immutable
storage，并对复制后的 payload 求 hash。源 padding 被省略，之后修改或释放源对象不能改变记录。

每次 commit 都生成新的 `ArtifactId` 与 `OutputCommitId`，即使内容逐字节相同。
`OutputCommitReceipt` 绑定：

- TenantId、JobId、JobSpecDigest、JobAttemptId；
- WorkerInstanceId 与 WorkerLeaseGeneration；
- OutputSlotId、ArtifactId 与 OutputCommitId；
- width、height、channels、scalar type、tight row bytes 与 payload bytes；
- ArtifactContentDigest；
- achieved `process-lifetime` durability。

按 ArtifactId 查询返回包含相同回执与 tight payload 的
`shared_ptr<const ArtifactRecord>`。没有 mutable record API，也不暴露 path、runtime
handle、IPC delivery id 或 store root。

该 store 不实现 filesystem publication、idempotency、quota、retention、TTL delivery、
restart persistence、recovery、atomic-visible 或 crash-durable commit。这些性质和稳定的
tenant-scoped identity allocation 属于 Issue #99。

## 边界与限制

- 模块是源码私有的，构建为 `photospider_single_tenant_job_internal`；不安装、不导出。
- `photospiderd` 与 protocol v2 不变；其 session、compute request、`OutputArtifactId` 和
  delivery id 仍是本地进程/传输值。
- 配置的单个 TenantId 不表示 authentication 或 authorization。
- Worker thread 与 Embedded Host 共享 caller 进程；没有 security、address-space、syscall、
  plugin 或 crash isolation。
- 只包含可信仓库 CPU operation；tenant plugin ABI 与隔离仍不存在。
- 一个 Job 只有一个 attempt 和一个必需 image slot；没有 retry、checkpoint、resource quota、
  durable retention 或 network API。
- 正数 JobSpec maximum parallelism 限制 Host Run，但不会 resize process execution worker，
  也不创建 server quota。

目标多进程安全模型仍由
[ADR 0011](../../adr/zh/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.zh.md)
和[服务端 roadmap](../../roadmap/zh/Kernel-Evolution.zh.md#服务器与插件隔离)
治理。

## 源码与长期测试入口

- 契约与规范摘要：`src/lib/server/job_contract.hpp` 和
  `src/lib/server/job_contract.cpp`。
- Job 真相与工件权威：`src/lib/server/single_tenant_job_service.hpp` 和
  `src/lib/server/single_tenant_job_service.cpp`。
- 真实 worker adapter：`src/lib/server/embedded_job_worker.hpp` 和
  `src/lib/server/embedded_job_worker.cpp`。
- 聚焦权威/生命周期测试：`tests/unit/test_single_tenant_job_service.cpp`。
- 真实 Embedded Host 纵向路径：
  `tests/integration/test_single_tenant_job_product_path.cpp`。

聚焦测试覆盖精确六字段规范字节与 SHA-256、path-shaped identity 拒绝、tight-row deep copy、
相同内容的身份分离、receipt-gated success、缺失 output、不匹配 lease fencing、返回 null/
异常的 factory 与 worker settlement，以及 cancel-before-commit 顺序。产品路径测试把
immutable graph identity 解析为微型 YAML 图，经新的 Embedded Host 执行、关闭、提交结果，并
查询身份完整的工件。
