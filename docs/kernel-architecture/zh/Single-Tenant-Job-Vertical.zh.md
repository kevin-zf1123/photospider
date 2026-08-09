# 单租户 Job 纵向路径

本文定义 `src/lib/server/` 下当前源码私有的 Issue #99 Job 行为。ADR 0011
仍是目标安全域决策，server roadmap 负责分配后续交付切片。当前模块是面向一个已配置
tenant 的真实单进程纵向路径；它不是 network server、multi-tenant authorization
boundary 或 OS-isolated worker manager。

## 当前剖面与身份

受支持的剖面是规范 `jobspec-v2`、`embedded-cpu-v1` execution 与
`crash-durable` image artifact。一个 `SingleTenantJobService` 拥有一个已配置
`TenantId`、一个可信 durable state root、一套有限 quota 配置、任意数量的 retained Job，
并为每个显式接受的 attempt 创建一个全新的进程内 worker object 与 Embedded Host。

身份域仍是彼此不同的强 C++ 类型：

| 身份 | 当前含义 | 明确不表示 |
| --- | --- | --- |
| `TenantId` | 一个 service/root 上唯一配置的 tenant | 已认证 principal 或 multi-tenant authorization |
| `JobId` | 一个已接受 durable Job 的稳定身份 | IPC compute id 或 Graph session |
| `JobSpecDigest` | 精确规范 `jobspec-v2` 字节的 SHA-256 | 单独构成 authorization |
| `JobAttemptId` | Job retry history 中一个不复用的 attempt | Job identity 或本地 Run identity |
| `WorkerInstanceId` + `WorkerLeaseGeneration` | 精确的全新进程内 worker assignment | PID、OS process、heartbeat 或 supervisor lease |
| `GraphArtifactId` | 由可信配置解析的 graph material key | Host path 或本地 `GraphSessionId` |
| `ArtifactId` | 稳定、不可变的 durable image identity | Content digest、path 或 IPC `OutputArtifactId` |
| `OutputCommitId` | 一个 Job output 的稳定幂等 transaction identity | Attempt identity 或 delivery lease |
| `ArtifactContentDigest` | 精确紧密 payload 字节的 SHA-256 | Artifact 或 commit identity |

初始 Job、artifact 与 commit id 包含抗碰撞 service namespace 和 checked local
sequence。Retry 保留 `JobId`、JobSpec digest、checkpoint、`ArtifactId` 与
`OutputCommitId`；它会生成全新的 attempt、worker、lease generation 与 quota
reservation。重启会恢复持久化身份，只为新提交的 Job 使用新的 namespace。

## 不可变 JobSpec 与 Checkpoint

`JobSpec` 只有 getter，没有 mutation surface。其构造函数接受：

- 一个有界 opaque `GraphArtifactId`；
- 一个非负 target node；
- 一个有界且必需的 `OutputSlotId`；
- 完整且全为正值的 `JobResourceRequest`，包含 CPU slot、host memory、output、
  staging、retention，以及按 configured-device 排序且唯一、最多 128 行的 vector；
- 可选的 durable checkpoint `ArtifactId`；
- 封闭的 `embedded-cpu-v1` execution profile；以及
- 封闭的 `crash-durable` durability request。

规范字节以 `jobspec-v2` 开头。十进制长度 framing 覆盖每个字段、每个 resource
scalar、每对有序 device label/byte，以及显式的 checkpoint presence/value。整数先转换为
规范十进制文本再 framing。构造函数保存精确字节的 SHA-256。Control plane 在接受前重新
校验，通过 `shared_ptr<const JobSpec>` 保留，并把 digest 绑定到每个 assignment 与
durable record。

JobSpec 不包含 Host path、descriptor、pointer、runtime handle、credential、IPC id、
plugin DSO、mutable store location、quota token 或 artifact mutation capability。Quota
admission 之前，control plane 只能通过同 tenant 已校验的 durable artifact index 解析可选
checkpoint。Worker 只获得只读 `ArtifactRecord`，永远不会获得 root/path 或 commit
authority。当前 Embedded CPU adapter 会校验该 provenance，但不声明恢复算法特定的
runtime state。

## Tenant Quota Authority

`TenantQuotaAuthority` 是该已配置 tenant 唯一的 server-side capacity authority。在一个
quota mutex 下，admission 把 concurrency、CPU、host memory、每个 configured device、
output、staging 与 retention 作为完整 envelope 检查。它要么发布一个 opaque
reservation 并完成全部 charge，要么什么都不改变。Worker、plugin、JobSpec 或
`ResourceLedger` token 都不能生成、放大或释放该 server reservation。

失败和取消的 attempt 恰好一次释放完整 envelope。成功 artifact commit 把已预留的
retention 转换为紧密 payload 的精确 charge，并释放所有 active-attempt dimension。Durable
artifact deletion 只有在 artifact directory、artifacts directory 与 root barrier 确认
manifest visibility removal 后，才释放 quota authority 所记录的精确 retained charge。
Visibility 尚未确认的失败会保留 charge；visibility 已确认时，即使之后的 private
payload/directory cleanup 失败也会释放 charge。启动时会从已校验 artifact 重建 retained
charge；如果 configured retention 低于 recovered data，则 fail closed。Active attempt
reservation 永不重建。

CPU slot 同时限制 Embedded Host `maximum_parallelism`。Host-memory 与 device 值是保守的
进程内 admission declaration，不是 OS memory/device enforcement，也不替代 worker-local
`ResourceLedger`；Issue #100 拥有该 process 与 OS-resource boundary。

## Durable Root、Job 与 Artifact

`DurableServerState` 会规范化一个可信 root，以 no-follow 方式打开，获取 exclusive
nonblocking process lock，保留 root/control/jobs/artifacts directory descriptor，并重新
校验 root device/inode binding。任何 JobSpec、report、checkpoint 或 plugin 都不能选择
path。一个 live authority 独占一个 root。

Job record 包含规范 JobSpec、current assignment 与 lease、稳定 artifact/commit id、
cancellation 与 terminal fact，以及成功时的 receipt。每次更新都会写入并同步 private file，
以 atomic rename 覆盖 authoritative record，再同步 jobs、control 与 root directory。
Recovery 会严格解析每条 record；含糊 entry 或 identity/content drift 会 fail closed。

Job-record replacement 有三种显式结果：

- `NotPublished`：atomic rename 尚未发生，先前 durable/cache truth 仍是 authority，caller
  可以执行普通回滚；
- `RecordPublishedDurabilityUnconfirmed`：rename 已令 replacement 可见，但所需
  jobs/control/root directory barrier 中至少一个失败；以及
- `ConfirmedCommitted`：全部所需 directory barrier 均已完成。

Serialization、filename 与 replacement-cache storage 都在 rename 前准备。Cache 会先在
durable-state mutex 下 swap 到 replacement，再执行 filesystem mutation；若结果为
`NotPublished`，则恢复旧 cache。Rename 之后 cache truth 已经对齐，不再留下 allocation 或
可能抛异常的 cache publication。任一 published state 的失败都不得转换为 rollback。
`SingleTenantJobService` 会保留已发布 snapshot 与 active quota、fence worker、拒绝后续
durable mutation，并要求重启。重启会重新校验 record；除非 stable artifact 证明成功，否则
仍存活的 nonterminal attempt 会被转换为 `RecoveryInterrupted`。

Artifact commit 会校验 server-owned request 与 CPU image，把 active row 复制成紧密
payload，校验 output/staging/retention bound，并 reconcile 已存在的 durable occurrence 或
安全 residue。新的 publication 随后会：

1. 准备完整的 private `ArtifactId` 与 `OutputCommitId` index 副本，并以可回滚方式同时安装；
2. 创建固定 opaque artifact directory；
3. 写入、同步、重新打开并 hash `payload.bin`；
4. 写入 private canonical manifest；
5. 以 no-replace 原子发布固定 `manifest` 名称，并立即令两个已安装 index 成为 authority；
6. 删除 private manifest；
7. 同步 artifact directory、artifacts directory 与 root。

Manifest presence 是 visibility point。Manifest 前的明确 residue 会被删除，两个 index
副本也会恢复；manifest 发布后，在后续任何可能抛异常的 cleanup、revalidation、observer 或
barrier 之前，按 `ArtifactId` 与 `OutputCommitId` 的直接查询都已经可用。Recovery 与 lazy
lookup 会校验 descriptor、payload length/digest、tenant/Job/spec/slot/artifact/commit join，
原子修复两个精确 alias，只清理安全 residue，并重新执行 barrier chain。使用相同 stable
commit 的 retry，只有在全部 stable identity、descriptor、digest 与 payload fact 匹配时才
返回原 receipt。Reporting attempt 可以不同，因为原始 acknowledgement 可能丢失。任何其他
collision 都会 fail closed。

Deletion 会先准备已移除目标的两个 index，然后报告四个不可逆状态之一：`NotRemoved`、
`ManifestRemovedDurabilityUnconfirmed`、
`VisibilityRemovalConfirmedCleanupPending` 或 `FullyCleaned`。Manifest 删除前失败会保留
两个 alias，mutation 仍可继续；manifest 一旦不存在，两个 alias 会同时撤销，因此 lookup 与
同进程 checkpoint admission 都不能暴露陈旧字节。Visibility durability 尚未确认时，service
保留 quota；完整 visibility barrier chain 完成后，即使 payload 或 directory cleanup 仍是可由
restart 清理的 residue，也会释放 quota authority 的精确 charge。Visibility 已变为不可逆后的
任何失败都会 fence worker 和后续 durable mutation，直至 restart。成功 Job 会保留历史
receipt，但删除后的字节不再能用于 lookup 或 checkpoint admission。

## Job State、Recovery 与显式 Retry

当前状态机为：

```text
submit -> Queued -> Running ---------------------> Succeeded
                    |                                 ^
                    +-> Cancelling -> Cancelled       |
                    +---------------> Failed --retry--+
restart(active, no matching artifact) -> Failed(RecoveryInterrupted)
restart(any non-cancelled state, matching stable artifact) -> Succeeded
```

`submit()` 会校验并冻结 JobSpec/checkpoint，预留完整 quota envelope，插入内存 Job 与
ownership record，在 service mutex 仍阻塞 worker progress 时启动唯一 assignment thread，
然后发布 accepted truth。因此 native-thread 启动失败发生在 durable publication 之前，不会
暴露 Job 或 handle。`NotPublished` journal failure 会移除 candidate 并释放其 quota；任一
published failure 会保留与 visible record 对齐的 Job、worker authority 与 quota，并进入单调
journal fail-stop。`query()` 复制当前 truth；`wait_for()` 只限制
observer wait，二者在 fail-stop 后仍可用。

`retry(JobId)` 只接受已经 settled、且没有 current worker/reservation 的 `Failed` Job。它
保留稳定 Job/spec/checkpoint/output truth，递增 lease generation，创建全新的
attempt/worker/quota authority，安装 replacement 与被阻塞的 worker，然后发布
replacement。`NotPublished` 会恢复先前 failed truth 并释放新 reservation；任一 published
outcome 都会保留新 attempt 与 reservation、fence worker progress，并 fail-stop 后续 durable
mutation。Report 必须匹配完整 current tenant/Job/spec/attempt/worker/lease tuple，因此 stale
attempt 会被 fence，不能 settle、fail、cancel 或 commit replacement。

Worker report 一旦通过 identity 与 semantic-shape fence，后续 control-plane durability
failure 就不会抹除其 outcome 或 settlement evidence。Manifest 发布前失败会成为已 settled
的 `Failed(ArtifactCommit)`，释放 active reservation，并保持可显式 retry。Manifest 发布后
失败则先重新校验 stable occurrence，将其 reconcile 为 `Succeeded`，且 retention 恰好 charge
一次。

重启绝不会恢复进程内 Graph/Run/Host/thread 或 ledger object。没有 matching committed
artifact 的 nonterminal durable record 会变为已 settled 的
`Failed(RecoveryInterrupted)`，并可显式 retry。Matching stable artifact 会被重新校验、
只 charge 一次并 reconcile 为 `Succeeded`，包括 manifest 已可见但 acknowledgement 或
Job-state update 丢失的 commit。Terminal receipt 内嵌于 Job record，因此 artifact 后续删除
后，历史 success 仍然存在。如果某个 stable artifact 位于 Job 预留的 output identity 下，
但 tenant/Job/spec/slot/commit join 中任一项不一致，recovery 会报告 durable corruption，
而不会采用或覆盖它。

## Worker、Cancellation 与 Completion Ordering

Embedded worker 会校验 assignment、JobSpec digest 与 optional checkpoint，在 JobSpec 外解析
graph material，创建并 seed 一个全新的 Embedded Host，加载 attempt-local Graph，在已预留
CPU parallelism 内 compute，校验一个非空 CPU image，关闭 Graph，销毁 Host ownership，
最后只返回 typed attempt fact 与 candidate image。

Worker report shape 与 full-tuple fencing 仍是封闭集合。Worker-owned failure fact 优先于
cancellation relabelling。来自旧 attempt 的 stale 调用会被忽略，不会改变 current retry；
来自 current attempt 的 malformed report 会变为 `ReportRejected`。`cancel()` 持久化
monotonic intent。先于 commit 获胜的 cancellation 会在 settlement 后丢弃成功 candidate；
先获胜的 durable commit 与 successful Job publication 不会被之后的 cancellation 改写。
如果 cancellation intent 未发布，先前 intent 仍是 authority；如果其 record 已可见、但后续
durability barrier 或 completion observer 失败，service 会保留 `Cancelling`、fence worker，
并进入相同 journal fail-stop。当前 public Host 没有 forced compute cancellation，因此忽略
cooperative observation 的 provider 仍可能无限期延迟 shutdown。

一个 private reaper 在 Job mutex 外 join 已完成 assignment thread。Destruction 会把 active
Job 标为 cancelling，并等待 worker/reaper drain。这只是有序的进程内 ownership：当前没有
WorkerManager process、heartbeat、crash/hang/OOM classification、address-space/syscall
isolation、forced termination 或 bounded shutdown。这些属性仍属于 Issue #100。

## 产品边界与持续维护证据

- 不安装、不导出的 `photospider_single_tenant_job_internal` target 与两个持续维护 Job test
  target 默认只存在于 Darwin 与 Linux。独立的
  `PHOTOSPIDER_BUILD_SINGLE_TENANT_JOB` gate 在其他系统默认关闭，拒绝在不支持的系统显式
  启用；CMake 会同时断言 enabled 与 disabled profile 的 target inventory。
- `photospiderd` 与 protocol v2 保持不变，不序列化这些 Job、quota、checkpoint 或 durable
  artifact contract。
- 配置的 `TenantId` 是可信配置，不是 authentication。
- 可信仓库 CPU operation 在 caller process 内运行。Tenant plugin ABI/network security 与
  isolated plugin runtime 仍不存在。
- 当前 artifact format 只是一个必需的紧密 CPU `ImageBuffer`，不是通用 runtime
  Value/checkpoint format 或 bulk data plane。

长期维护入口包括：

- contract：`src/lib/server/job_contract.{hpp,cpp}`；
- quota：`src/lib/server/tenant_quota.{hpp,cpp}`；
- durable state：`src/lib/server/durable_server_state.{hpp,cpp}`；
- control plane：`src/lib/server/single_tenant_job_service.{hpp,cpp}`；
- Embedded adapter：`src/lib/server/embedded_job_worker.{hpp,cpp}`；
- focused authority/lifecycle test：
  `tests/unit/test_single_tenant_job_service.cpp`；以及
- 真实 Embedded Host durable product path：
  `tests/integration/test_single_tenant_job_product_path.cpp`。

持续维护测试覆盖 canonical digest/validation、共享的 128-device admission/recovery 上限与
129-device rejection、每个 quota dimension 与多设备核算、精确 settlement、所有 Job-record
publication/barrier fault stage 的内存与重启 truth、manifest 前后 failure、manifest
前双 index preparation rollback、manifest 发布后立即按 OutputCommitId 查询、root
lock/no-follow/identity drift、safe cleanup、corruption 与精确 Job/artifact recovery join、
idempotent reconciliation、所有 artifact-deletion fault stage 的双 alias 撤销、精确 quota、
fail-stop 与 restart cleanup、同进程 deleted-checkpoint rejection、checkpoint
authorization/re-authorization、显式 retry 与 fresh fencing、submit/retry thread-start
rollback、submit/retry/cancel journal fail-stop、interrupted/successful restart、cancellation
ordering、stale/malformed report、持续 thread reaping、target-inventory platform gating，以及
真实 Embedded Host output/checkpoint/restart 行为。

目标 multi-process model 仍由
[ADR 0011](../../adr/zh/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.zh.md)
和 [server roadmap](../../roadmap/zh/Kernel-Evolution.zh.md#服务器与插件隔离)治理。
