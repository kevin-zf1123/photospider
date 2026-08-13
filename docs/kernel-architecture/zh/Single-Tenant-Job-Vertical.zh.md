# 单租户 Job 纵向路径

本文定义 `src/lib/server/` 下当前源码私有的 Issue #99/#100/#105 Job 行为。ADR 0011
仍是更广泛的目标安全域决策，server roadmap 负责分配后续交付切片。当前模块是面向一个
已配置 tenant、带全新 exec attempt process 与 attempt-scoped stream artifact
data plane 的真实本地纵向路径；它不是 network server、multi-tenant authorization
boundary、独立 WorkerManager/artifact service、remote data plane 或 untrusted-plugin sandbox。

## 当前剖面与身份

受支持的剖面是规范 `jobspec-v2`、`embedded-cpu-v1` execution 与
`crash-durable` image artifact。一个 `SingleTenantJobService` 拥有一个已配置
`TenantId`、一个可信 durable state root、一套有限 quota 配置、任意数量的 retained Job，
并为每个显式接受的 attempt 创建一个全新的 worker process 与 Embedded Host。Service
authority 内一个源码私有 `WorkerManager` 拥有每个 process 与 supervision handle。

身份域仍是彼此不同的强 C++ 类型：

| 身份 | 当前含义 | 明确不表示 |
| --- | --- | --- |
| `TenantId` | 一个 service/root 上唯一配置的 tenant | 已认证 principal 或 multi-tenant authorization |
| `JobId` | 一个已接受 durable Job 的稳定身份 | IPC compute id 或 Graph session |
| `JobSpecDigest` | 精确规范 `jobspec-v2` 字节的 SHA-256 | 单独构成 authorization |
| `JobAttemptId` | Job retry history 中一个不复用的 attempt | Job identity 或本地 Run identity |
| `WorkerInstanceId` + `WorkerLeaseGeneration` | 精确的全新 process assignment 与 manager-fenced lease | Raw PID capability、Job identity 或单独构成 authorization |
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
checkpoint。Worker 通过 control 只获得 receipt、descriptor、digest、size 与一个不授权的 join
reference，随后只从继承的 checkpoint data descriptor 重建只读 `ArtifactRecord`。它永远不会
获得 root、path、quota token、稳定 output transaction 或 commit authority。当前 Embedded CPU
adapter 会校验该 provenance，但不声明恢复算法特定的 runtime state。

Checkpoint authorization 与 control-frame capacity 无关。精确 durable bytes 与 receipt 必须和
已配置 tenant 及 JobSpec `ArtifactId` 完整连接，payload 还必须适配已接受的 host-memory
envelope。Worker-side materialization 会接收精确声明的 byte count 及随后的 stream EOF，并在
Graph construction 前重新校验 descriptor 与 SHA-256。任何不匹配都会被拒绝，且不能从该
stream occurrence 推导出 quota、Job、retry 或 artifact authority。

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

Active-attempt release 会在首次 mutation 前校验完整 subtraction，并提供强异常保证。如果
release 抛出异常，service 会把精确 reservation owner 保留在 terminal Job control 上；若
submit/retry rollback 没有 durable Job，则转移到一个 service-owned stranded slot。随后它会
单调 fail-stop submit、retry、cancellation、worker report、artifact deletion 和其他所有
durable mutation。Query、bounded wait、artifact lookup 与 quota inspection 仍然可用。同一
进程内，service 不会重试 release，也不会发布补偿性的 terminal record。Restart 会丢弃
process-local active reservation、重建 durable Job/artifact truth，并清除此 fail-stop。

CPU slot 同时限制 Embedded Host `maximum_parallelism`。WorkerManager 会在 `exec` 前把已
接受的 host memory 应用为 worker 的 POSIX `RLIMIT_AS` soft bound；它限制 total virtual
address space 而非实测 RSS，因此 executable 及其 runtime mapping 必须容纳于请求的
envelope。作为独立的 worker-filesystem 防御，WorkerManager 会准备一个与 accepted output/staging/retention
最小值精确相等的 `RLIMIT_FSIZE` soft bound。若继承的有限 hard limit 更低，所属 attempt 会在
`fork` 前以 `WorkerStartup` 失败；它绝不会静默缩窄已经从 JobSpec 派生出的 stream data-plane
maximum。该限制不是 transport，也不存在 file-backed fallback。Configured device 值仍是
admission declaration，不是 device isolation。两者都
不替代 worker-local `ResourceLedger`；当前不存在 syscall filter、cgroup/container、GPU
memory enforcement 或 hostile-plugin sandbox。

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

1. 准备完整的 private `ArtifactId` 与 `OutputCommitId` index 副本，以及一个以 ArtifactId 为键的
   durability-confirmation 副本，然后以可回滚方式同时安装三者；
2. 创建固定 opaque artifact directory；
3. 写入、同步、重新打开并 hash `payload.bin`；
4. 写入 private canonical manifest；
5. 以 no-replace 原子发布固定 `manifest` 名称，并立即令两个已安装 alias 成为 authority 且可识别，
   同时让它们共享的 confirmation state 保持 pending；
6. 删除 private manifest；
7. 同步 artifact directory、artifacts directory 与 root，然后在确认完成 acknowledgement 前，
   于同一 mutex 下记录 confirmation。

Manifest presence 是 visibility point。Manifest 前的明确 residue 会被删除，两个 index
及 confirmation 副本也会恢复。Manifest 发布后，在后续任何可能抛异常的 cleanup、validation、
observer 或 barrier 之前，两个 alias 都已经成为 authority 并可在内部识别，因此不会只索引
其中一个 alias，也不会把任一 alias 误报为 absent。但在 confirmation 仍为 pending 时，它们
还不能向外返回 artifact/receipt。首次 `ArtifactId` lookup、`OutputCommitId` lookup、
same-commit retry 或 service reconciliation 必须重新加载并校验精确 descriptor、payload
length/digest、tenant/Job/spec/slot/artifact/commit join，并重放完整 artifact-directory、
artifacts-directory 与 root barrier chain。只有随后一次在锁内进行且不抛异常的 confirmation
transition 才允许返回 crash-durable 结果。Confirmation 会先于最终 completion observer，
所以 root barrier 后丢失 acknowledgement 仍会保留 confirmed truth。Recovery 与 lazy repair
会把两个精确 alias 及 confirmation 作为一个 transaction 安装。使用相同 stable commit 的
retry，只有在全部 stable identity、descriptor、digest 与 payload fact 匹配时才返回原
receipt。Reporting attempt 可以不同，因为原始 acknowledgement 可能丢失。任何其他
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

`Running` 是耐久 control-plane fence，表示仍为当前值且未取消的 assignment 已开始接受
manager supervision。它在 external process spawn 前发布，因此不能证明 exec、
`AssignmentAccepted`，也不能证明 worker 已进入 heartbeat/cancellation loop。长期真实进程
cancellation 证据会改为等待一个 source-private、无权威能力的“首个合法 heartbeat 已观察”
信号。合作 fixture 与忽略取消 fixture 使用独立 service instance 及分支局部的 heartbeat/
cooperative bound，因此为 responsive worker 提供 scheduler 余量不会拖慢短时 forced
escalation 分支。该 observer 不改变任何产品 deadline，也不暴露 identity、PID、descriptor、
signal、wait、reap、cancellation 或 completion authority。

`submit()` 会校验并冻结 JobSpec/checkpoint，预留完整 quota envelope，插入内存 Job，
并在 service mutex 仍阻塞 assignment progress 时请求 WorkerManager 构造并保留唯一 manager
record 与 supervision handle，然后发布 accepted truth。Bulk data-plane 创建、checkpoint 传输与
output 传输只会在已登记 supervision thread 拥有 attempt 后、该 service mutex 之外发生。因此 manager-record 构造、registry
插入或 supervision-thread 启动失败发生在 child spawn 和 durable publication 之前，不会暴露
Job 或 handle。Supervision-thread 只会在 `records_.emplace()` 成功后开始构造，并与一个
确定性的 source-private start-failure seam 共用同一 catch 边界；任一异常都会先删除该精确
record，再由 submit/retry 执行 Job 与 candidate-quota 回滚。长期测试会捕获“此前已插入”的
证明，观察异常后 manager ownership 为零，再证明后续 submit/retry 能恢复。`NotPublished`
journal failure 会移除 candidate
并释放其 quota；任一
published failure 会保留与 visible record 对齐的 Job、worker authority 与 quota，并进入单调
journal fail-stop。如果 manager-record/thread start 或 `NotPublished` rollback 无法释放 quota，
candidate Job 仍未发布，精确 reservation owner 会转移到 service 的 stranded slot，原始
submit error 会被重新抛出，后续所有 mutation 在 restart 前均被 fail-stop。`query()` 复制
当前 truth；`wait_for()` 只限制
observer wait，二者在 fail-stop 后仍可用。

`retry(JobId)` 只接受已经 settled、且没有 current worker/reservation 的 `Failed` Job。它
保留稳定 Job/spec/checkpoint/output truth，递增 lease generation，创建全新的
attempt/worker/quota authority，保留被阻塞的 manager record，然后发布
replacement。`NotPublished` 会恢复先前 failed truth 并释放新 reservation；任一 published
outcome 都会保留新 attempt 与 reservation、fence worker progress，并 fail-stop 后续 durable
mutation。Report 与 manager action 必须匹配完整 current tenant/Job/spec/attempt/worker/
lease tuple，因此 stale
attempt 会被 fence，不能 settle、fail、cancel 或 commit replacement。
如果 manager-record/thread start 或 `NotPublished` retry rollback 无法释放 fresh reservation，先前 failed
Job truth 仍是 authoritative，fresh owner 会转移到 stranded slot，触发 retry 的 error 会被
重新抛出，并且同一 fail-stop 生效，不会在同一进程内重试。

Worker report 一旦通过 identity 与 semantic-shape fence，后续 control-plane durability
failure 就不会抹除其 outcome 或 settlement evidence。Manifest 发布前失败会成为已 settled
的 `Failed(ArtifactCommit)`，释放 active reservation，并保持可显式 retry。Manifest 发布后
失败则先重新校验并重放 pending barrier chain，再将 stable occurrence reconcile 为
`Succeeded`，且 retention 恰好 charge 一次。一旦观察到精确匹配的 artifact truth，或者
lookup/revalidation 仍存在 manifest-visible 歧义，后续任何 barrier replay、quota conversion
或 `Succeeded` Job-record publication failure 都会进入单调 reconciliation fail-stop。它绝不会
写入补偿性的 `Failed/ArtifactCommit` record，也不会释放仍然有效的 reservation。Quota
conversion 失败时保留 active reservation；conversion 成功但随后 Job journal 在 publication
前失败时保留 retained charge。Worker 以及后续 report/mutation 都会被 fence，直至 restart
重建并 reconcile 当前最强 durable truth。

Failed、Cancelled、`ReportRejected`、malformed-report 和 manifest 发布前
`ArtifactCommit` terminal publication 之后，也适用同一 release-failure 规则。不能只因 quota
settlement 抛出异常就重写 durable terminal truth；reservation 仍有 owner，所有 mutation 与
后续 report 都会被 fence，restart 会以零 active reservation 恢复已记录的 terminal state。

重启绝不会恢复进程内 Graph/Run/Host/process 或 ledger object。没有 matching committed
artifact 的 nonterminal durable record 会变为已 settled 的
`Failed(RecoveryInterrupted)`，并可显式 retry。Matching stable artifact 会被重新校验、
只 charge 一次并 reconcile 为 `Succeeded`，包括 manifest 已可见但 acknowledgement 或
Job-state update 丢失的 commit。Terminal receipt 内嵌于 Job record，因此 artifact 后续删除
后，历史 success 仍然存在。如果某个 stable artifact 位于 Job 预留的 output identity 下，
但 tenant/Job/spec/slot/commit join 中任一项不一致，recovery 会报告 durable corruption，
而不会采用或覆盖它。

## Worker、Cancellation 与 Completion Ordering

产品 composition 会在 JobSpec 外、service ownership 建立前解析可信 graph material，并把它
保留在 immutable `PreparedExternalGraphCatalog` 中。Catalog 构造会在保留每个 entry 前，对
`root_dir`、`yaml_path`、`config_path`、`cache_root_dir` 与 `message` 应用 private protocol
唯一的 16-KiB 文本字段 byte 上限。精确 16 KiB 有效；多一个字节会在 factory/service 构造前
同步抛出指出具体字段的 `std::length_error`，因此也先于 DurableServerState、quota、Job、
supervision-thread、channel 或 process ownership。失败 constructor 不暴露部分 catalog，也不会
变成之后的 `WorkerStartup` fact。WorkerManager 首先登记一个 immutable manager record 与
supervision handle。只有该 supervision owner 会创建两条 private `AF_UNIX SOCK_STREAM` lane，把
checkpoint lane 缩减为 manager-send/worker-receive，把 output lane 缩减为
worker-send/manager-receive，并把两个 manager endpoint 设为 nonblocking。该 setup、所有 bulk
传输与每个 failure 都发生在 service mutex 之外；setup failure 只退役 owning attempt，并关闭
所有 endpoint，不留下 path、staging file 或 artifact residue。Supervisor 随后 fork/exec 一个
不安装的 `photospider-worker`，先登记其精确 PID，再通过不可覆写的内存 catalog lookup 把
material 复制进精确一个 immutable assignment。它不调用 resolver，也不执行 graph/path 或
data-plane filesystem I/O。Fork child 只执行 descriptor
setup、`RLIMIT_AS`、`RLIMIT_FSIZE`、descriptor closure 与 `exec`；全新 exec 的 worker 会校验
Assignment metadata 与 JobSpec digest，通过 fd 5 接收并校验 optional checkpoint，创建并 seed
一个全新的 Embedded Host，打开并加载 attempt-local Graph，在已预留 CPU parallelism 内
compute，通过 fd 6 发送紧密 candidate row，关闭 Graph，销毁 Host ownership，最后只返回 typed
attempt 与 staged-output metadata。

Exec 前的 descriptor ownership 是精确的：fd 0-2 是标准 stream，fd 3 是 private control
socket，close-on-exec fd 4 用于向 parent 传递 setup `errno`。Darwin parent 在 `fork` 前查询
内核 `kern.maxfilesperproc` 上界。Fd 5 是 worker 的 receive-only checkpoint lane，fd 6 是
send-only output lane；manager 保留两条 stream 各自相反的 direction。不分配内存的 child 关闭 `[7, 上界)`
中每个 slot，只把
worker 对 checkpoint lane 的反向 send 与 manager 对 output lane 的反向 send 会在本地失败，
且不会因 SIGPIPE 终止：Linux 使用 `MSG_NOSIGNAL`，Darwin 在两个 endpoint 上安装
`SO_NOSIGPIPE`，可移植的拒绝集合是 `EPIPE`、`ECONNRESET` 或 `ENOTCONN`。两个允许方向都
保留精确 byte 与 EOF 语义。
`EBADF` 视为未使用 slot。当前 soft `RLIMIT_NOFILE` 不是安全边界，因为已经打开的高位
descriptor 会在限制随后降低后继续存在。Linux child 使用 raw
`close_range(7, UINT_MAX, 0)`；包括旧内核不提供 syscall 在内的任何错误都会通过 fd 4 报告，
并令 startup 失败。这里不存在任意有限 fallback 或从 `RLIM_INFINITY` 到 `INT_MAX` 的
userspace 扫描。长期进程回归会在隔离 authority 中保持一个高位 non-close-on-exec
sentinel，同时覆盖 infinite soft limit 与把 soft limit 降到既有 sentinel 以下两种情况，
并证明及时 exec、sentinel 不被继承，同时 parent 副本仍保持打开。

Parent-side WorkerManager descriptor 使用与 fork-child closure sweep 不同的 close 规则：
`UniqueFd` 会先替换或清除 ownership，再恰好调用一次 `close`，并忽略包括 `EINTR` 在内的
每种结果。Linux 可能在报告 interrupted close 前已经释放并重新分配该数字 fd，因此重试可能
关闭另一个 thread 新取得的 descriptor。一个 source-private callback 回归会强制形成这种
release/reuse 顺序，并证明不会有第二次 close 消耗复用后的 descriptor。

Worker exec bootstrap 要求 `--control-fd`、`--checkpoint-data-fd`、
`--output-data-fd`、`--startup-timeout-ms` 与 `--io-timeout-ms`。WorkerManager 在 fork 前准备
这些 string。worker 把精确 configured startup duration 用于初始 Assignment receive/checkpoint
materialization，并把精确 I/O duration 用于 acceptance、heartbeat 与 Report write；worker
本地默认值或 cap 无法缩短 manager policy。Bootstrap descriptor 与 startup 不进入 control
payload，因为它们是在第一帧可用前就需要的 process capability/policy。

parent 也会把该 configured startup duration 作为同一个 absolute exec-bootstrap deadline 应用
于 fd 4。其 nonblocking reader 会跨 `EINTR`/`EAGAIN` 保留 partial native-`int` state；
partial-record EOF 仍是 truncated setup failure。Poll/read ready 不等于接受：读取一份完整 child
`errno` 或干净 close-on-exec EOF 后，WorkerManager 必须取得 fresh monotonic observation，且仅
在 `now < deadline` 时暴露结果。等值或更晚是 typed `WorkerStartup` deadline，并且优先于
child-error 或 exec-success 分类；既有精确 PID owner 会执行 TERM→KILL→`waitpid` 清理。之后
另行创建的 Assignment startup window 不能复活迟到的 exec 结果。长期真实进程测试会对两种
完整结果确定性地跨越该边界，并要求 process、thread、quota、receipt 与 artifact residue 全部
归零。

源码私有 duration 域会在取得 durable ownership 前封闭。九个 `WorkerManagerOptions` 字段都
为正，且不大于包含式共享上限 `4,294,967,295 ms`。`heartbeat_interval` 采用更窄的包含式
上限 `4,294,967,294 ms`，并且必须保持严格小于 `heartbeat_timeout`；这也保留了 protocol 的
无符号 32-bit 毫秒 cadence。构造过程会在打开 durable root 前逐个校验具名字段，exec argument
构造与解析则独立执行 startup/I/O 上限。受支持的 Darwin/Linux monotonic clock 能精确表示每个
已接受毫秒值。每个 deadline 只捕获一次 base，检查
`base <= time_point::max() - duration`，再与同一个 base 相加；clock-range 耗尽会抛出
`std::overflow_error`，而不求值一个溢出的 sum。因此 `milliseconds::max()` 无法进入 conversion
或 deadline arithmetic，validation 也不会相对于之后的时钟观察变得陈旧。

Private bounded protocol 现在具有固定 magic、唯一支持的 version 2、封闭 message kind、
128-KiB control-payload 上限、deadline-aware partial I/O，以及严格的 trailing-byte、enum、
identity、digest、descriptor 与 Job-resource 校验。Version 1 会被拒绝，没有 compatibility
decoder 或 bulk fallback。唯一的 source-private `kMaximumWorkerTextFieldBytes` 常量在 catalog
admission、Assignment/Report encoding 与 decoding 中统一约束五个 prepared graph string 及
Report diagnostic。精确上限是包含式的；本地 prepared value 超界是 `std::length_error`，wire
content 超界则是 `WorkerProtocolError`。

Assignment 传输完整 attempt/JobSpec 与有界 graph metadata、optional checkpoint receipt/
descriptor/digest/size/reference、output-stage reference/maximum 与 cadence。Report 传输 outcome
fact，并且只在 settled success 时传输 output reference/slot/descriptor/size/digest。两者都不能
编码 checkpoint bytes、candidate row、`ImageBuffer`、blob、`Value`、path 或 raw file
descriptor。完整声明的最大 metadata envelope 能放入 control 上限。只要符合已接受的
output/staging/retention
与 file-size envelope，超过原 64-MiB aggregate frame 上限的 candidate 仍然有效。file-size
limit 会与 accepted maximum 精确匹配；若继承的有限 hard limit 更低，则会在 fork 前成为
`WorkerStartup`，而不是一个更小的 runtime envelope。超过 resource envelope 会变成一个保留
identity、已 settled 的 `Failed(Compute)` metadata Report，携带固定
有界 diagnostic 与空 stage；不存在 transport-size fallback。

WorkerManager 只有在收到 current-identity Report、精确 stream EOF、clean zero exit、精确 reap
与 control-channel EOF 后才暴露 staged output。worker 在 output byte 前发送
reference/descriptor/exact-size/digest metadata，并保留 source，同时让真实 heartbeat thread
保持活跃。当精确 child 仍受 lifecycle ownership 约束且尚未 reap 时，manager 会创建一份与最终
image size 精确一致、惰性、无 path 的匿名 owner，并在每次 monitor 迭代中最多把一个 64-KiB
nonblocking slice 直接接收到该 owner，同时强制 accepted maximum 并增量计算 SHA-256。每个后续
slice 都要先经过 cancellation/shutdown/runtime/heartbeat 仲裁。包括连续或预缓冲 byte 在内的
output progress 都不能续期或复活 heartbeat deadline；只有合法且 current-identity 的 Heartbeat
frame 可以。不存在累计 accumulator 扩容、whole-payload reconstruction copy 或 post-reap bulk
access。Reference、slot、tight descriptor、精确 stream byte count、resource bound 与 digest 必须
在 completion handoff 前全部连接。worker 只在精确 bytes 后关闭 output lane，并保持存活且可被
终止，直到 manager 完成关联并以 O(1) 移动已经是最终形态的 image owner，再返回一次匹配且只含
identity 的 `CompletionReady`。该确认不授予 Job、quota、artifact、commit 或 publication
authority。若先前的 cancellation-channel failure 使回复不可能，已经完整关联的 Report 可以
保留其普通分类。Post-reap drain 只处理 control metadata：它绝不读取 bulk lane，也不执行
filesystem I/O、blocking data transfer、bulk allocation 或 content hashing。不匹配会成为
worker-protocol failure，不能生成 receipt 或 Job/quota/retry truth。只有
既有 service 与 `DurableServerState` 才能通过 manifest-last durable transaction 发布稳定
ArtifactId/OutputCommitId truth。

长期维护的真实进程证据先让一份大于 64 MiB 的 candidate 进入可读状态，再使其保持 pending，
跨越一个完整 heartbeat timeout。source-private、无 authority 的 manager observation 会证明
第二帧或更晚的合法 current-identity Heartbeat 在该阶段被接受。配对 fixture 只发送首帧合法
Heartbeat 时，必须以 `WorkerHeartbeatTimeout` 失败，且不留下 receipt、artifact、quota、
process、thread 或 descriptor residue。产品构造不包含该 observation；它不能改变 liveness、
ownership 或 publication。

Manager 与 worker 的短 poll loop 都会为自己的 channel 保留一个 decoder：deadline 到期会保留
partial 或 complete header/payload byte 与精确 offset，而 clean EOF 只在 fresh frame boundary
上有效。Socket readiness budget 与 semantic lifecycle acceptance 彼此独立。Output pending
期间，已经到期的 budget 会在一个 bulk slice 前只执行一次 nonblocking control probe；它不授权
late frame。只有 monotonic time 严格早于最早适用的 absolute lifecycle deadline 时，frame 才
对调用方可见；正向 read、完整 decode，以及 Assignment、`AssignmentAccepted`、Heartbeat、
Report、Cancel 或 `CompletionReady` interpretation 后都会复查。Control write 会在正向 send
progress 前后检查同一严格边界。Progress 后的 timeout 可能表示 peer 已收到 prefix 或 final byte，
因此调用方必须将该 write 视为失败，并且绝不重试该 frame。Cancellation owner 只能为有界
receive-side report/EOF/exit 排空继续保留 channel。

WorkerManager 独占 spawn、private channel、PID、signal delivery、`waitpid` 与 supervision-
thread reaping。任何 API 都不接受或暴露 PID。每条 cancellation 或 signal 路径都会重新校验
完整 tenant/Job/spec/attempt/worker/lease record 及其保留 PID。Candidate report 只有在一次
clean worker exit、channel closure 与精确 reap 之后，才有资格交给 control plane 裁决。
Startup/exec、nonzero exit、signal death、channel loss、protocol violation、heartbeat
timeout、runtime timeout 与 forced-cancellation fact 使用彼此分离的 durable failure category，
且只影响拥有它的 attempt。
产品构造会在打开 durable root 前拒绝 `SIGCHLD=SIG_IGN` 与 `SA_NOCLDWAIT`，每次 spawn 还会
在 `fork` 前立即重新校验 `SIGCHLD` 是否保持可等待。之后若策略被修改、出现竞争 reaper，或
精确 `waitpid` 返回任何非 `EINTR` 错误（包括 `ECHILD`），就表示精确回收授权丢失；authority
会在任何 completion callback、completed-record 标记或 record 删除前 fail-stop。仍保留 live
PID 的 record 绝不能被标记为 complete 或删除。

仅仅完成精确 reaping 同样不能退役 manager record。Assignment begin 一旦成功或抛出异常，
WorkerManager 就会在各自的 no-throw 构造边界内构造每个实际首次 `Report`（外部进程或显式
in-process test marker）、`Failure` 与 `ForcedCancellation` completion，并且控制面 callback
必须在 `mark_completed` 前返回。如果 fault injection、identity/message/report 保留、wait-status
格式化或返回值构造抛出异常（包括 `std::bad_alloc`），该局部边界不会调用 callback，而是立即
进入固定的 allocation-free fail-stop；异常不能逃逸到外层通用分类器再被改写成第二个 failure
completion。如果 callback 抛出异常，则不会重试，因为它可能已经部分应用 durable truth。
两种情况都会在 completed-record 标记或普通 record 删除之前 fail-stop。它们绝不伪造
replacement completion，也不释放 service-owned quota reservation。这次 fail-stop 后，重启仍是
durable Job 与 quota owner 唯一的 reconciliation 边界。Begin callback 返回 false 是唯一合法
的不带 completion 退役路径，因为它在 worker 执行前就 fence 了未发布或已被替换的 assignment。

Worker report shape 与 full-tuple fencing 仍是封闭集合。Worker-owned failure fact 优先于
cancellation relabelling。来自旧 attempt 的 stale 调用会被忽略，不会改变 current retry；
来自 current attempt 的 malformed report 会变为 `ReportRejected`。`cancel()` 持久化
monotonic intent。先于 commit 获胜的 cancellation 会在 settlement 后丢弃成功 candidate；
先获胜的 durable commit 与 successful Job publication 不会被之后的 cancellation 改写。
如果 cancellation intent 未发布，先前 intent 仍是 authority；如果其 record 已可见、但后续
durability barrier 或 completion observer 失败，service 会保留 `Cancelling`、fence worker，
并进入相同 journal fail-stop。Intent 被接受后，WorkerManager 先发送精确 cooperative
cancellation。发送失败会继续有界排空 report/EOF/wait status，并保留真实 Failed report、
nonzero exit、signal death 或 channel close；发送失败本身不会 mint forced cancellation。
Cooperative deadline 时仍存活的 worker 会先被关闭/撤销 channel，再在 configured bound 下接收
owner-validated `SIGTERM`/`SIGKILL` escalation，最后精确 reap。Deadline 决策会再执行一次
精确的 nonblocking exit observation。如果该观察在 channel 撤销前 reap 了自然退出，reaping
不得被当作 channel EOF：WorkerManager 会在独立且有界的 post-reap drain 期间保留 parent
socket 与 stateful decoder，使已经进入缓冲区的 Report 与 EOF 仍按普通 report/channel/exit
truth 分类。该路径不发送 signal、不执行第二次 reap，也不能仅因 cooperative deadline 已到就
产生 forced cancellation。在已经尝试 cancellation delivery 后发生 socket-system read error
时，会在同一个有界 monitor 内把 channel 标记为 unavailable。后续 decode 停止，但 process
ownership、cooperative/escalation deadline 与精确 reap observation 继续，因此 signal/nonzero
wait status 或已经 decode 的 Report 优先于 `WorkerChannel`。只有没有 Report 的 clean zero
exit 仍是 channel failure。当 cooperative deadline 仍然有效时，普通 EOF/post-report
deadline 必须服从它，不能先通过 generic channel path 终止并 reap worker。因此，精确 exit
status 或 manager-owned escalation 会先完成裁决，之后才允许剩余 `WorkerChannel` fact。
当对应 worker 仍然存活时，完整、有效的 candidate Report 不会削弱这一顺序：普通
post-report close/exit deadline 不能先终止或 reap worker。在 cooperative deadline 之前观察到
的 worker-owned signal 或 nonzero exit 仍是权威事实；只有在该 deadline 到达时仍然存活的
worker 才会进入 cancellation 所有的 `SIGTERM`/`SIGKILL` escalation，并且才可能被分类为
forced cancellation。
只有匹配 owner 已成功发送
`SIGTERM` 或 `SIGKILL` 的精确 `WIFSIGNALED` 状态才能产生 forced-cancellation fact。对已经
退出的 zombie 调用 `kill()` 成功不构成因果证明；正常零退出仍按 report/channel/exit truth
分类。Destruction 会记录 cancellation，且不在 Job mutex 下等待，随后通过相同 escalation path
并发排空所有 attempt。Reap observation 在最终 kill/reap deadline 前保持非阻塞；如果仍无法
观察到精确 reaping，authority process 会 fail-stop，而不是无限阻塞或带着 live ownership
返回。

由于全部 filesystem open 与 graph load 现在都发生在 exec 后、精确保留 PID 的约束下，阻塞
的可信 I/O 会进入同一 escalation path。长期 FIFO process fixture 证明有界 forced
cancellation 与精确 reaping、有界 service 析构，以及 material 可读后由 fresh worker 成功
执行。不可覆写的 prepared catalog boundary 则另外移除了可能无限占住 manager reaper 的
pre-PID resolver callback。

一个 private reaper 会在 manager 与 Job mutex 之外 join 已完成的 supervision handle。显式
源码私有 test marker 可以在 supervision thread 内执行 deterministic unit-test worker；它不
安装，也不声明 process-isolation 或 bounded-termination。普通构造会拒绝未标记的
in-process factory、不存在/不可执行的 worker path，或把产品进程配置为自动回收 `SIGCHLD`
child 的策略。

## 产品边界与持续维护证据

- 不安装、不导出的 `photospider_single_tenant_job_internal` 与 `photospider-worker`
  target、protocol/manager unit coverage、process fixture/supervision integration coverage 与
  Embedded product-path coverage 默认只存在于 Darwin 与 Linux。独立的
  `PHOTOSPIDER_BUILD_SINGLE_TENANT_JOB` gate 在其他系统默认关闭，拒绝在不支持的系统显式
  启用；CMake 会同时断言 enabled 与 disabled profile 的 target inventory。
- `photospiderd` 与 local IPC protocol v2 保持不变，不序列化这些 Job、quota、checkpoint 或
  durable artifact contract；private worker protocol v2 是另一个源码私有 wire。
- 配置的 `TenantId` 是可信配置，不是 authentication。
- 此纵向路径中的可信仓库 CPU operation 在 attempt process 内运行。Tenant plugin
  ABI/network security、syscall isolation 与 isolated hostile-plugin runtime 仍不存在。
- 当前 artifact format 是一个必需的紧密 CPU `ImageBuffer`。本地 direction-reduced stream adapter 是
  针对该 format 的 attempt-scoped bulk data plane，不是通用 runtime `Value` format、remote
  transport、object store 或 standalone artifact service。

长期维护入口包括：

- contract：`src/lib/server/job_contract.{hpp,cpp}`；
- quota：`src/lib/server/tenant_quota.{hpp,cpp}`；
- durable state：`src/lib/server/durable_server_state.{hpp,cpp}`；
- control plane：`src/lib/server/single_tenant_job_service.{hpp,cpp}`；
- Embedded adapter：`src/lib/server/embedded_job_worker.{hpp,cpp}`；
- private worker transport 与 lifecycle：
  `src/lib/server/worker_protocol.{hpp,cpp}`、
  `src/lib/server/worker_artifact_data_plane.{hpp,cpp}` 和
  `src/lib/server/worker_manager.{hpp,cpp}`；
- 单 assignment composition root：`apps/photospider_worker/main.cpp`；
- focused authority/lifecycle test：
  `tests/unit/test_single_tenant_job_service.cpp` 与
  `tests/unit/test_worker_protocol.cpp`；
- real-process lifecycle fixture 与 integration coverage：
  `tests/support/photospider_worker_fixture.cpp` 与
  `tests/integration/test_worker_supervisor.cpp`；以及
- 真实 Embedded Host durable product path：
  `tests/integration/test_single_tenant_job_product_path.cpp`。

持续维护测试覆盖 canonical digest/validation、共享的 128-device admission/recovery 上限与
129-device rejection、每个 quota dimension 与多设备核算、精确 settlement 与强异常保证的
release fault、所有 Job-record
publication/barrier fault stage 的内存与重启 truth、manifest 前后 failure、manifest
前双 index preparation rollback、manifest-visible pending lookup/retry barrier replay 与 replay
failure、root barrier 后 acknowledgement 丢失、quota-conversion reconciliation fail-stop、
连续 `Succeeded` pre-publication journal failure、worker/report fencing 与 restart quota truth、
root lock/no-follow/identity drift、safe cleanup、corruption 与精确 Job/artifact recovery join、
idempotent reconciliation、所有 artifact-deletion fault stage 的双 alias 撤销、精确 quota、
fail-stop 与 restart cleanup、同进程 deleted-checkpoint rejection、checkpoint
authorization/re-authorization、显式 retry 与 fresh fencing、submit/retry manager-record/thread-start
rollback、submit/retry/cancel journal fail-stop、interrupted/successful restart、cancellation
ordering、stale/malformed report、submit/retry manager-record/thread start 与 `NotPublished` rollback 的
release-failure ownership、Failed/Cancelled/rejected/malformed/pre-manifest terminal truth、
read-only availability、report/mutation fencing 与 restart convergence、持续 handle/process
reaping、target-inventory platform gating、bounded protocol reconstruction、fresh process
identity、crash/protocol/heartbeat/runtime isolation、全部九个 duration 在 durable ownership 前的
超界拒绝、精确共享 duration 与 heartbeat 关系边界、checked monotonic deadline range、FIFO-held
fresh-retry stale-lease rejection、
在首个 heartbeat rendezvous 与分支局部 bound 后验证 cooperative/forced cancellation、
cancel-channel-versus-wait-status attribution、deadline-side
natural-reap buffered-report drainage、candidate-Report-deadline-versus-wait-status attribution、
最大声明 metadata-envelope accounting、protocol-v1 rejection、control metadata 与 bulk size
独立性、使用真实 staged byte 的 reference/descriptor join 与 stale attempt 拒绝且无
process/quota/receipt/artifact/staging residue、checkpoint 与 candidate 跨越原 aggregate
control bound 的 data-plane 传输、暂停 checkpoint 传输时 service cancellation 仍响应、worker
因暂停 output drainage 而 backpressure 时 shutdown 仍有界、真实进程
中五个 prepared graph 字段达到共享精确边界、在 service ownership 前针对具体字段同步拒绝
多一个字节且不留下 durable-root/Job/quota/thread/process residue 或 `WorkerStartup`、真实进程
resource 超界 typed failure、data-plane setup rollback 且不留下 path 或 stage、在隔离进程中
拒绝 finite-hard-`RLIMIT_FSIZE` 且不留下 process/quota/artifact residue，以及
retry/restart/new-Job 对 checkpoint identity、digest、durability 与 quota truth 的保持、
concurrent shutdown drainage、实际首次 completion/重建
allocation fail-stop、completion callback 异常 fail-stop，
以及真实 Embedded Host output/checkpoint/restart 行为。

Issue #106 新增一个纯产品 `decode_worker_assignment` boundary，以及一个针对有界 worker
control metadata、手工 opt-in 的本地 parser robustness harness。成功 decode 必须重新编码为同一
canonical frame；socket receiver 会委托给该 decoder，并保留绝对 acceptance deadline。Harness
不会传输 artifact byte，也不会创建 descriptor、worker、process、quota、receipt 或 publication
authority。确定性的 canonical、prefix、truncation、trailing-byte、identity、digest、descriptor
与 heartbeat 行为由已注册测试负责，而不是由手工 harness 负责。

这一本地 Issue #99/#100/#105 可执行子集，加上 Issue #106 validation 与 observation layer，不会
新增 network/multi-tenant control plane、独立部署的 WorkerManager 或 artifact service、
remote/authenticated data-plane capability、untrusted plugin sandbox 或 Issue #125 Metal 工作。
这些更广泛边界仍由
[ADR 0011](../../adr/zh/0011-server-control-plane-workers-and-plugin-runtimes-are-separate-security-domains.zh.md)
和 [server roadmap](../../roadmap/zh/Kernel-Evolution.zh.md#服务器与插件隔离)治理。
