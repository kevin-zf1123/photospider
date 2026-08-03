# ADR 0010：执行画像 SLO 是六项独立 Benchmark 判定

## 状态

作为 Issue #92 与路线图跟踪键 S-1 的目标契约接受。本 ADR 冻结 Issues #93 至
#96 要消费的 `execution-profile-slo-v1` workload、测量、证据与判定契约。

Issue #92 是一项决策与文档变更。它不新增 runtime profile enum、public API、
benchmark result field、collector、workload harness、CTest entry、CI 性能门禁或
当前行为声明。后续 Issue 必须先实现并验证各自负责的证据行，才能把这些目标
提升到当前架构文档。

## 背景

进程执行域已经区分 Interactive 与 Throughput QoS。当前 Host policy 会执行
deadline 优先、可信 work/ready-byte 计费、分层 Graph/Run 公平性、八次 dispatch
aging、两个 class 都可启动时最多三次 Interactive start 后一次 Throughput start，
以及 Interactive admission headroom。这些是排序与准入机制，不是端到端执行画像
SLO。

当前测量证据范围更窄：

- `BenchmarkService` 重复 Host compute，并报告平均 wall time、operation time
  截尾平均、平均 I/O time 和解析后的 Run 并行度。它没有 warmup、percentile、
  稳定 output digest、completed-work window、discarded-work counter 或 memory
  high-water 契约。
- `opencv_operation_concurrency_benchmark` 是长期维护的手工工具，针对一个合成
  OpenCV graph 提供 warmup、原始 wall sample、MPix/s 中位数、speedup 与最大
  callback overlap。其既有快照不是永久性能 baseline。
- 长期测试已经证明 Run cap 1/2/4/8 的精确 callback overlap、一个 cap-1/cap-8
  fixture 的逐位相等、确定性的 policy ordering、3:1 class-start 上界、headroom
  accounting、cancellation isolation 与精确资源释放。
- `ExecutionLifecycleTelemetry` 保留有界 transition record、service-relative
  monotonic timestamp、identity 与 lifecycle counter。它不暴露 queue wait、
  completed work、Host/device byte、result digest 或无损历史；cursor gap 会使重建
  无效。
- `ResourceLedger` 暴露权威 Host 与已配置 device dimension 的 current/limit
  snapshot，但当前 public benchmark 路径不会保留逐 workload high-water sample。

[ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)
要求这些当前事实与本目标和实时 Issue 状态分离。
[ADR 0003](0003-process-owned-execution-resources.zh.md) 与
[ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)
要求 isolated 与 mixed 行使用同一个进程自有执行权威，而不是为每个画像建立隐藏
pool。[ADR 0009](0009-compute-io-durability-and-completion-semantics.zh.md)
要求 B1 throughput 等待其独立要求的 artifact commit，不能把 provider return 当作
完成。

## 决策

### 四个不可变 Workload 共用一个精确 Graph Family

v1 source 是生成的 2048x2048 RGBA FP32 Value。对从零开始的坐标 `x`、`y`、
channel `c` 与 unsigned eight-bit seed，每个存储的 binary32 sample 是下式按
round-to-nearest-ties-to-even 得到的值：

```text
((17*x + 31*y + 47*c + seed) mod 256) / 255
```

Graph 串行应用四个仓库自有 `image_process:curve_transform` node，baseline
`k` 依次为 `0.80`、`1.00`、`1.20`、`1.40`，第四个 node 是 target。精确的
仓库 OpenCV HP tiled provider 在 Host CPU 上执行这些 node。一个 logical
site-operation 表示一个 transform 处理一个 RGBA pixel site，与 channel 数量
无关。生成的 source byte、规范化 graph 与
parameter value、所选 operation/provider binary 与 generation，以及后续全部
payload 都在 workload manifest 中按内容寻址。

规范 workload matrix 如下：

| Workload | 冻结行为 |
| --- | --- |
| `I1-edit-storm-v1` | 使用 seed zero 与自然序号为 `1..12` 的十二次 edit。对 `0..11` 中的 `edit_index = edit_ordinal - 1`，第一个 node 的 `k` 从 `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]` 取值，source Region 为 `(256*(edit_index mod 4), 256*floor(edit_index/4), 256, 256)`。每个 Run 使用 `ComputeIntent::GlobalHighPrecision`、`ComputeRunQuality::Full`、Interactive QoS、weight 1、Run cap 8、相对 150 ms monotonic deadline，以及精确的 `(Graph, target node four, GlobalHighPrecision)` supersession key。只有第十二次 edit（`edit_index=11`、`k=1.04`、Region `(768,512,256,256)`）必须发布；它获得 500 ms drain。 |
| `I2-progressive-v1` | 复用精确的 I1 source、graph、seed、edit ordinal、source-space Region 与 realtime request lineage。512x512 preview source 是对 2048 source 逐 channel 执行 4x4 box average、只舍入一次到 binary32 后，再经过相同四个 transform；preview Region `edit_index` 为 `(64*(edit_index mod 4), 64*floor(edit_index/4), 64, 64)`。Final 计算 2048 source。只有第十二次 edit（`edit_index=11`、preview Region `(192,128,64,64)`）具有必需的 preview 与 final latency result，且必须按此顺序出现；stale output 不得发布。 |
| `B1-immutable-v1` | 包含 immutable job `0..29`；job `n` 使用 source seed `n`、baseline graph、Throughput QoS、weight 1、无 deadline 或 supersession、精确 reservation 证据、canonical semantic trace、crash-durable committed artifact 与按 job index 区分的 logical/raw golden。偶数 job 属于 Graph A，奇数 job 属于 Graph B。测量边界上 harness 同时提供两个有序的 15-job queue，且不会暂停非空 queue；由有界 Host admission 而非 harness 决定驻留多少个 Run。Run cap 1 与 8 是两行独立的必需证据。 |
| `M1-shared-v1` | 在 measured time zero 启动 I1，此后每 750,000,000 ns 重复一次，共精确启动 40 个 episode；同时循环执行精确的 B1 corpus，保留偶数/奇数 Graph 分配、Run cap 8 与持续 offered backlog，共测量 30 秒。两条 stream 共用一个 `ExecutionService`、worker set、ready store、policy binding set 与 `ResourceLedger`；不得用隐藏 pool、重复 ledger 或独立进程承接任一 stream。 |

对 fairness 而言，只要某个 Graph 的 producer 仍有未消费的 offered demand 且没有
暂停提交，该 Graph 就是 *eligible*。这段 workload-level interval 包含等待有界
admission 的时间；它不声明全部 30 个 B1 Run 同时被准入。每个 Graph 内部按递增
job index 提供工作，前一个 job terminal 时 producer 同步提供下一个。M1 启动新的
`0..29` cycle 时不得产生 producer-side gap。

#### Edit ordinal 与 monotonic cadence 均为精确契约

自然语言中的 edit 编号始终表示 `1..12` 中的 `edit_ordinal`；公式、数组、Region、
lineage record 与 evidence 使用 `0..11` 中从零开始的 `edit_index`，并满足
`edit_index = edit_ordinal - 1`。诸如“edit 12”这样的裸短语不是 v1 identity。
必需 final 始终写作“第十二次 edit（`edit_index=11`）”。其 coefficient、source
Region、preview Region、lineage、logical digest、metric sample 与 evidence
record 都携带同一个 index。

对一个 episode，harness 只有在 reset baseline 已物化并结算后，才选择 monotonic
origin `E`。`edit_index=i` 的名义 admission-call start 为：

```text
S_i = E + i * 16,666,667 ns,  i in 0..11
```

Harness 禁止在 `S_i` 之前启动 Host admission call。记录的实际 start `A_i` 必须
满足 `S_i <= A_i <= S_i + 2,000,000 ns`；这是有界 start-lateness 规则，并不声称
操作系统会在精确纳秒醒来。提前启动、迟于 2 ms 启动、admission failure、dropped
edit 或 cadence-event gap 都会使 replicate 无效。Missed edit 不会迟到补交：harness
取消该 episode 的剩余部分，记录 missed/drop/gap 事实，并且绝不追赶、回填或移动
后续名义时刻。

在每个 cold、warmup 或 measured phase 内，episode origin 精确为
`E_r = E_0 + r * 750,000,000 ns`。Reset/baseline 准备必须在 `E_r` 前完成；否则
该 episode 无效，不能滑动 schedule 或插入未记录的 cooling delay。M1 对
`r=0..39` 使用 `E_r = M_0 + r * 750,000,000 ns`。因此，配对的 isolated 与 mixed
证据共享相同 v1 schedule、start-lateness bound 与 miss/drop/gap 规则，但不声称
具有相同 physical wake time。

#### I2 冻结一个目标 progressive state machine

该 state machine 是分配给 #94 的目标 benchmark-harness 语义。它不宣称 #92
新增了当前 public API，也不宣称当前 caller 已暴露 progressive publication。“相同
I1 lineage”表示相同 Graph/target/revision 与 ordinal-to-generation mapping；它不会
复用 I1 的 `GlobalHighPrecision` canonical request key。对每个
`edit_index`，目标 harness 在合法 realtime request key
`(Graph, target node four, ComputeIntent::RealTimeUpdate)` 下生成下一个非零
generation。它立即启动一个 preview child，使用 `ComputeIntent::RealTimeUpdate`、
`ComputeRunQuality::Interactive`、Interactive QoS、weight 1、Run cap 8，以及该
edit 记录的 Host admission start 后 100 ms 的 absolute steady-clock deadline。
它同时 arm 一个 final child，使用 `ComputeIntent::GlobalHighPrecision`、
`ComputeRunQuality::Full`、Interactive QoS、weight 1、Run cap 8，以及同一起点后
1,000 ms 的 absolute deadline。两个 child 都携带 realtime request
`SupersessionIdentity`，因此 HP child 的 compute intent 仍与其 canonical request
key 分离，符合当前 `ComputeRunSubmission` 契约。

Final child 只在该 edit 的 preview 首次可见、且其 generation 仍为 current 时精确
提交。若更新的 edit 先被接受，armed final 不经提交即被丢弃；已经提交的 preview
或 final 被 supersede，且只能 drain。新 generation 会撤销两个较旧 child 的
publication permission。Stale terminal event 可以更新 cleanup、waste 与 quiescence
证据，但不能发布 `Value`、digest、receipt 或必需 latency result。第十二次 edit
（`edit_index=11`）必须先发布 preview、再发布 final；failure、deadline expiry、
反序、重复 publication，或在两个 endpoint 完成前出现更新 generation，都会使
episode 无效。较早 generation 只能在仍为 current 时发布，并不属于必需 result。

Preview latency 从 preview Host admission call 前立即开始，到 current preview
可见时结束。Final end-to-end latency 使用同一起点，到 current final 可见时结束；
较晚的 final Host admission time 作为 diagnostic trigger timestamp 单独保留。因此
final 门禁包含 preview、trigger、admission、execution 与 publication，不会隐藏
preview interval。

I2 具有必需的 Host-local output path 与条件式 Metal residency 组件。Preview 与
final 都向同一个本地 consumer 两次暴露各自不可变的 CPU `ValueRevisionId`、Host
binding/allocation identity 与 storage byte；两次获取必须复用同一个 binding，不得
发生 CPU copy。存在 `DeviceId(DeviceBackend::Metal, 0)` 时，每个不同
preview/final revision 的第一次 access 可以执行一次精确大小的 Host-to-Metal
transfer，第二次必须复用同一个
device-local residency，且 transfer 与 allocation 都为零。禁止 Metal-to-Host
transfer、filesystem/codec I/O，以及上述两个条件式首次 access 之外的任何 transfer。
没有 Metal 时，只有 device-specific 组件属于预定义 `not-applicable`；Host reuse
与 no-I/O 门禁仍然适用。第十二次 edit（`edit_index=11`）的 final logical digest
必须等于 I1 `edit_index=11` digest，preview logical digest 必须等于其自身 fixture
golden。

每个必需 logical output digest 都通过调用 `compute_content_digest(Value)` 得到。
Sample 只有在返回的 `ContentDigestResult.state` 为
`ContentDigestState::Available`、`digest` 存在，且 `digest->algorithm` 为
`CanonicalDigestAlgorithm::Sha256CanonicalV1` 时才有效。Evidence 记录 algorithm
tag 与小写十六进制 `ContentDigest.bytes`。任何其他 state、缺失 digest、provider/
readiness failure 或不同 algorithm 都会使受影响行成为 `invalid`。这个 canonical
logical `ContentDigest` 不是 artifact raw byte 的 SHA-256。

每个 B1 job 在全新的可丢弃 job 目录下提交两个文件。Payload
`output.rgba32le` 是紧密 row-major RGBA，sample 为 little-endian IEEE-754
binary32。`manifest.txt` 使用无 BOM UTF-8 与 LF（包括末行之后），并精确包含
以下固定顺序字段：

```text
schema=execution-profile-artifact-v1
job=<unpadded decimal 0..255>
width=2048
height=2048
channels=RGBA
scalar=ieee754-binary32
byte-order=little
row-stride=32768
payload=output.rgba32le
payload-sha256=<lowercase 64-hex SHA-256>
```

v1 payload byte count 通过
`2048 * 2048 * 4 * 4 = 67,108,864` checked computation 得到。对每个有效 job
`0..255`，精确 manifest 长度为 `242 + decimal_digit_count(job)` byte：job `0..9`
为 243 byte，job `10..99` 为 244 byte，job `100..255` 为 245 byte。因此 measured
job `0..29` 使用 243 或 244 byte，cold/warmup job `252..255` 使用 245 byte。每个
job 的目标 durable-output owner 使用进程自有 `ComputeIoExecutor` 执行两个有序
task，稳定 charge identity 分别为 `(job, payload-stage, attempt)` 与
`(job, manifest-commit, attempt)`。Payload-stage task 声明
`planned_bytes=67,108,864`；manifest-commit task 声明与该 job 精确 manifest 长度
相等的 `planned_bytes`。每次 `try_submit` 前都必须通过 checked arithmetic 得到这些
值，同一 identity 的全部 attempt 必须使用相同 charge。64-task 与
268,435,456-byte summed-planned-byte limit 适用于每次 accepted admission。

`planned_bytes` 是 task-retained byte 的稳定 admission estimate。它不是 physical
allocation measurement、memory ownership 证明；同时，它是 Compute I/O admission、
snapshot high-water 与 final settlement 的强制性权威证据。它不能替代 process RSS
或 ledger/device ownership evidence。Capacity rejection 让已 offered job 保持
eligible、同一个 task 保持 pending；所有 admission attempt 与 typed status 都会
保留。在无故障 B1 中，每个 task 只能被接受并启动一次，不允许 output retry、
duplicate task 或改变 charge identity。

Payload-stage task 在结算前必须完整写入、hash、同步并重新验证 private payload
stage。只有这样，manifest-commit 才可以写入并同步 private canonical manifest，
以 no-replace 方式 atomically publish，重新验证 published identity，并执行所有
leaf-to-root directory barrier。B1 请求 typed `crash-durable` durability，并且只接受
typed achieved `crash-durable`；只有 atomic visibility 并不成功。不支持 file
synchronization、directory barrier 或 atomic no-replace publication、achieved class
较弱，或者任一 transaction failure，都会使 job 无效，且不得生成成功的
crash-durable receipt。

`OutputCommitReceipt` evidence 至少绑定稳定 `OutputCommitId`、rooted namespace/
output slot、job index、descriptor 与 logical content identity、committed version/
generation、payload 与 manifest 名称、精确 byte count 与 raw SHA-256、requested 与
achieved durability，以及 published manifest identity。只有所有请求的 barrier
成功后才能返回 receipt。这是 ADR 0009 的目标 `OutputStore` authority，不是当前
private IPC delivery store，也不是 #92 对 runtime behavior 的扩展。

每个 B1 artifact destination，无论采用显式 disposable path 还是 release-artifact
storage，都必须位于一个已选 `OutputStore` root 或 rooted namespace 之下，且该
root 请求的 `crash-durable` 能力必须成功。Remote、RAM-backed、copy-on-write 或
其他 nonlocal root 不会仅因名称被拒绝，但也绝不预设为 durable 或可比较。Bundle
保留所选 root/path 拼写、解析后的 root 与 mount identity，以及每个 job directory
都位于该 root 之下的证明。这些 path fact 是审计证据；临时 absolute path 或全新
job-directory name 不是兼容键。

每个 B1 或 M1 行都设置 `storage_environment_applicability=required`，并记录一个
规范化、可散列的 `execution-profile-storage-environment-v1` fingerprint。它至少
包含：

- `OutputStore` provider/backend identity 及适用的 generation 或 version；
- backend class、显式 local/remote locality，以及 volatile/nonvolatile persistence
  class；
- filesystem type、稳定 mount identity，以及会影响 file sync、directory sync、
  atomic no-replace、rename、barrier 与 copy-on-write 行为的规范化 mount option 和
  semantics；
- durability capability set，以及 requested 与可证明 achieved durability class；
- 稳定的 backing volume/device/storage identity 和 storage class，或 provider 特有
  的等价 identity；以及
- hardware write-cache 与 power-loss-protection policy，二者分别携带显式 known、
  unknown 或 schema 规定的 not-applicable state。

每个必需 fact 都是 typed observation，其 state 为 `known`、`not-applicable`、
`unknown`、`unobserved`、`unsupported` 或 `unprovable`。只有在 schema 规定了
reason、且有证据证明该层不在端到端 durability path 中时，`not-applicable` 才
有效。规范化 object 保留在 raw evidence 中；其规范
`execution-profile-storage-environment-v1` serialization 使用 SHA-256 计算为小写
`storage_environment_digest`。只有当全部必需 fact 都为 `known` 或有依据的
`not-applicable`、capability set 证明必需 operation、且 requested 与 achieved
durability 均为 `crash-durable` 时，fingerprint 才具备 compatibility eligibility。
即使 byte 或 digest 相等，相同的 `unknown`、`unobserved`、`unsupported` 或
`unprovable` state 也绝不因此变得兼容。

Raw payload SHA-256 hash 精确的 67,108,864 个 little-endian byte；manifest
SHA-256 hash 该 job 的精确 `242 + decimal_digit_count(job)` 个 canonical byte（在
有效范围内为 243、244 或 245 byte）。按 job index 区分的
golden fixture 分别绑定 expected typed logical `ContentDigest`、expected raw-payload
SHA-256 与其自身 content-addressed golden identity。这三类 identity 绝不能互相
替代。一个 B1 job 只有在 Run success、有效 crash-durable receipt，以及两个
golden comparison 均成功后，才到达唯一 throughput completion endpoint。Isolated
interval 在最后一个 job 的 golden-verification completion 结束，而不是 provider
return、Run terminal、payload close、manifest rename 或 atomic visibility。

第一次下游 fixture 实现只能物化上述选择并计算 hash。改变 source 公式、operation、
coefficient、edit、preview filter、Graph 分配、cadence、必需 output 或 semantic
manifest 时，必须创建新 workload id。既有 v1 证据绝不被覆盖。

### Reference Protocol 分离 Cold、Warm 与 Mixed 证据

每一行使用三个全新的 process/execution-domain replicate。Warmup 前，每个
replicate 都要记录并冻结：

- repository commit 与 dirty-state declaration、build type、compiler、flag、
  OS/kernel、CPU/GPU/device inventory 和 power/thermal eligibility；
- provider/plugin binary hash 与 generation、固定 process worker count、Run cap、
  workload/fixture hash、seed、cache 与 residency precondition；以及
- 全部 resource limit 与 Interactive headroom；以及
- 对 B1 与 M1，所选 `OutputStore` root evidence、规范化 storage fingerprint、
  `storage_environment_digest`、compatibility eligibility 与 raw capability
  observation。

v1 resource configuration 是 32 个 CPU slot、1 GiB Host retained memory、
512 MiB Host scratch、65,536 个 ready entry、256 MiB ready byte；Interactive
headroom 为 1 个 CPU slot、64 MiB retained memory、32 MiB scratch、1,024 个
ready entry 与 16 MiB ready byte。Compute I/O 准入上限为 64 个 task 与 256 MiB
计划字节总量。配置 Metal 时，其 device-memory 与 scratch limit 分别为 512 MiB
与 256 MiB。Metal 缺失属于预定义 `not-applicable`，不是零观测。

B1 evidence 在每次 accepted task admission 与每次 task settlement 后立即采样
`ComputeIoExecutor::snapshot()`，并保留一个 pre-row 初始 sample 与一个
post-quiescent 最终 sample。它记录 task charge identity、planned byte、admission
status、completion status、active-task count 与 active-planned-byte count。每个
active-planned-byte total 都是对真实 per-job charge 的 checked sum，其 high-water
是这条完整 event-aligned stream 的最大值；缺少任一 sample、算术不一致、值超过
冻结 limit 或最终 count 非零，都会使该行无效。最终 snapshot 的 active task 与
active planned byte 必须都精确为零。

Cold、warmup 与 measured work 都禁用 disk-cache/codec I/O 和跨 episode/job 的
result reuse。I1/I2 只保留显式重新计算的 baseline、当前 episode target，以及 I2
已声明的 output residency；每个 B1 job 开始时都没有其 fixture identity 的可复用
result。Cold 与 warmup 观测也是精确契约，不由 harness 自行选择：

| Workload | Cold diagnostic | Warmup | 每个 replicate 的 measured 证据 |
| --- | ---: | ---: | ---: |
| I1 | 1 个 episode | 20 个 episode | 200 个 episode |
| I2 | 1 个 episode | 10 个 episode | 100 个 episode |
| B1 | 每个必需 Run cap 使用 seed 252 的 job | 每个必需 Run cap 使用 seed 253、254 与 255 的 job | 每个必需 Run cap 使用 job `0..29` |
| M1 | 1 个 mixed second | 5 个 mixed second | 30 个互不重叠的一秒 window |

Warmup B1 job 使用相同 graph 与完整 artifact path，但采用 warmup-only identity
与目录。其 owner 结算后移除 warmup/cold output；保留 process/provider/JIT state。
测量边界重置 counter 但不重启进程，M1 会重启 cadence，并在 measured time zero
启动第一个 episode。Cold first use 单独保留，绝不混入 steady-state aggregate。
全部 duration 使用 monotonic clock。Percentile 使用 nearest rank：排序 `N` 个
sample，并选择从一开始的 rank `ceil(p*N)`。每个 replicate 必须独立通过；不得
通过 pooling 隐藏坏进程。摘要可以报告三个 replicate aggregate 的中位数。

### 每个 SLO 维度拥有不可替代的独立判定

每个必需维度输出 `pass`、`fail`、`invalid` 或 schema 预定义的
`not-applicable`。证据缺失、checked-arithmetic overflow、monotonic-clock failure、
telemetry gap/drop、fixture/environment drift、未固定或不兼容的 reference，以及
未经批准的 `not-applicable`，都会使该行成为 `invalid` 且不符合要求。Composite
score、平均值、start count、RSS sample 或更快的维度都不能替代另一项判定。

#### Latency

I1 latency 从 final edit 的 Host admission 前立即开始，到匹配的 current
generation 可见时结束。I2 使用上面 state machine 中两组显式 start/end boundary。

- I1 final-generation p50/p95/p99 必须分别不超过 50/100/150 ms，且每个 measured
  episode 都必须发布 final generation。
- I2 第十二次 edit（`edit_index=11`）first-preview p50/p95/p99 必须分别不超过
  50/75/100 ms；第十二次 edit 的 final p95/p99 必须分别不超过 500/1000 ms。
  两个 endpoint 都必须匹配各自必需的 logical `ContentDigest`。
- M1 必须满足 I1 绝对 limit，且其 p99 不得超过配对 isolated I1 p99 的 2.0 倍。

被取消的中间 generation 不进入成功 percentile。Accepted-cancel-to-physical-
quiescence duration 作为独立观测保留。

#### Throughput

Throughput 是每秒成功的 logical site-operation，以 MPix-op/s 报告。一个 B1 job
只有在 Run success、crash-durable output receipt 与 logical/raw 两种 golden
verification 完成后才精确贡献 16,777,216 个 site-operation。其 isolated interval
从两个 measured queue 被提供前立即开始，到最后一个 job 的 golden-verification
completion 结束。Candidate
与 reference replicate 按 ordinal 配对：三个 candidate/reference ratio 的中位数
必须至少为 0.95，且每个 ratio 至少为 0.90。
每个 ordinal 的 candidate 与 reference B1 行都必须使用下文定义的兼容 storage
fingerprint。同一 subject 内 B1 cap-1/cap-8 determinism 比较也要求相同且兼容的
fingerprint；Run cap 是有意存在的差异，storage 不是。

对 M1，每个一秒 mixed B1 rate 除以配对 isolated cap-8 replicate 实测的 B1 rate。
Nearest-rank p05 ratio 必须至少为 0.20。Denominator 缺失或为零时结果为
`invalid`。

#### Fairness

对 Graph A 与 Graph B 在整个一秒 window 都 eligible 的每个窗口，令两者的
completed charged service 为 `x_A` 与 `x_B`：

```text
J = (x_A + x_B)^2 / (2 * (x_A^2 + x_B^2))
```

Nearest-rank p05 Jain index 必须至少为 0.95。总 service 为零的窗口属于
`invalid`。Charged service 使用 Host policy 单位
`work_units + ceil(ready_bytes/4096)`。

两个 class 都持续 startable 时，最多三次 Interactive start 后必须出现一次
Throughput start。M1 还要求：因 Throughput 消耗已声明 headroom 导致的
Interactive admission failure 为零、Interactive latency 门禁通过、Throughput
progress floor 为 0.20。Start order、completed progress、headroom admission 与
latency 是彼此独立的证据。

#### Determinism

对全部三个 replicate、fresh-process restart 和 Run cap 1 与 8 中相同的 B1 job
index，下列每一项 mismatch count 都必须为零：

- typed logical output `ContentDigest`；
- raw little-endian payload SHA-256；
- canonical artifact-manifest SHA-256；
- immutable 且按 job index 区分的 logical/raw golden identity；以及
- `execution-profile-semantic-trace-v1` fingerprint。

Semantic trace 对 deterministic plan 中的每个 logical task 精确包含一条 `ready`、
一条 `start` 和一条 `terminal` record。`task` 是 deterministic plan traversal
分配的从零开始且连续的 plan ordinal，不是 physical start order。每条 record 包含
`job`、Graph role、`task`、`action`、按数值排序的 dependency ordinal、terminal
outcome，以及 task 声明的 `work_units`、ready entry/byte、CPU slot、Host
retained/scratch byte 与 device-memory/scratch byte。无故障 B1 要求 terminal
outcome 为 `succeeded`；非 terminal record 使用 outcome `-`。

Canonical byte 以以下精确 ASCII header 与 LF 开始：

```text
execution-profile-semantic-trace-v1
```

其后每条 record 都是使用以下字段顺序的一行精确 ASCII：

```text
job=<u>;graph=<A|B>;task=<u>;action=<ready|start|terminal>;deps=<u,...|->;outcome=<succeeded|->;work=<u>;ready-entries=<u>;ready-bytes=<u>;cpu=<u>;host-retained=<u>;host-scratch=<u>;device-memory=<u>;device-scratch=<u>\n
```

显示的 `\n` 表示一个 LF byte（`0x0a`），不是 backslash 与 `n` 两个 byte。
全部 unsigned integer 都使用不补零的 decimal（`0` 是零的唯一写法），不存在
whitespace 或 BOM，dependency 使用逗号分隔的递增 ordinal 或 `-`，每一行（包括
末行）均以 LF 结束。Record 按 numeric job、Graph `A` 在 `B` 之前、numeric task，
然后按 `ready < start < terminal` 的 action rank 排序。Fingerprint 是这些精确 byte
的 lowercase hexadecimal SHA-256。

缺少或重复必需 record、task ordinal 不连续、dependency target 缺失、未知/额外
field 或 action、非法 outcome、encoding violation 或 event-collector gap，都会让
trace 无效，而不仅是不同。Timestamp、duration、physical worker/thread/device
queue identity、全局 mint 的 Run/task id、raw sequence number、queue position、
retry 与 physical start/completion order 都被排除。未 canonicalize 的 physical
trace（包括上述 diagnostic）仍单独保留。因此，semantic record set 可以比较 cap
1、cap 8 与 fresh replicate，而不会编码它们允许不同的 physical completion order。

跨环境带容差比较是兼容性证据，不能满足这项同环境精确判定。

#### Waste

Started-service waste 为：

```text
discarded_started_service / all_started_service
```

Started service 使用 `work_units + ceil(ready_bytes/4096)`。分子包含每个因
cancellation、supersession、failure、duplicate execution 或 retry 而无法 commit
结果的已启动 callback。在 cancellation 或 supersession 被接受后才启动的 work
单独计数，并且必须精确为零。已经进入的不可抢占 work 必须一直计费到 drain。

每个 I1 与 I2 replicate 的 Interactive discarded-service ratio 必须不超过 0.25。
M1 对 Interactive service 单独应用相同 ratio，避免 completed B1 service 稀释它。
I2 还要求 filesystem/codec byte 为零、CPU-copy byte 为零，并且除上述两个条件式
首次 Host-to-Metal access 外的 transfer/allocation byte 为零。无故障 isolated 或
mixed B1 的 discarded、duplicate 与 retry service 必须为零。

#### Memory

Memory 证据保留 Host retained memory、Host scratch、ready byte 以及每个已配置
device 的 memory 与 scratch 的 byte high-water mark，并保留 row-owned
post-quiescent reservation/grant delta。B1 还保留 event-aligned
`ComputeIoExecutor` active-task 与 active-planned-byte high-water 及其精确零结算。
B1 planned-byte stream 是 Compute I/O admission、planned-byte high-water 与 final
settlement 的强制性权威证据；它不证明 physical memory ownership，也不能替代 RSS
或 ledger/device ownership evidence。任何权威 dimension 都不得超过冻结 limit。
Isolated 行必须精确结算到 row 前 baseline；M1 shutdown 必须结算到零。

对每个权威 dimension，candidate B1 与 I2 peak 必须不超过已固定同环境 reference
的 105%，同时仍满足绝对 limit。Process RSS 只作为 diagnostic，因为它包含当前
authority 之外的 allocation；它不能替代 ledger/device 证据，也不能免除 settlement。

### 证据按内容寻址并采用 Fail-Closed 规则

每个 measured row 都属于一个 `execution-profile-slo-v1` bundle。Bundle 包含全部
冻结来源、raw sample/event、eligibility window、drop/gap counter、output/artifact/
trace/golden digest 与 commit receipt、transfer/copy/residency 证据、high-water
sample、aggregate input 与 result、独立 verdict，以及 typed comparison/pairing
reference。Eligibility 表示上文定义的 offered-demand interval。单位、公式、
denominator 定义和 invalidation reason 是 schema field，不能只写在说明文字中。

每个 bundle 记录 `subject_role=candidate|reference`。Candidate 的
`comparison_reference_bundle_digest` 选择用于 candidate/reference regression 的
immutable external baseline；它不是 M1 isolated denominator。Candidate 与 comparison
reference 必须具有相同 evidence schema、workload id、environment class、resource
configuration 与 fixture hash。二者的 repository/build identity 可以不同，并且必须
记录，因为这正是 comparison 的 subject。

Environment class 按行确定适用范围，而不是一个没有限定的 machine label。
`base_environment_digest` 绑定 OS/kernel、architecture、CPU/GPU/device inventory、
compiler/build mode 与 flag、worker count、provider/plugin generation、冻结 resource、
cache/residency precondition 和 power/thermal eligibility，但不绑定 repository commit。
逐行 `environment-class digest` 对 base digest、
`storage_environment_applicability`，以及 applicability 为 `required` 时具有
compatibility eligibility 的 `storage_environment_digest` 一并计算 hash。I1 与 I2
把 storage applicability 设为 `not-applicable`，且不携带 storage digest，因为其
必需路径不执行 `OutputStore` artifact commit；B1 与 M1 把它设为 `required`。
规范化 fingerprint 与 raw observation 仍保留在 bundle 中，使 reader 可以复算
两个 digest。

Storage compatibility 要求 fingerprint schema 相同、每个规范化 field 精确相等、
独立复算的 `storage_environment_digest` 相等，并且两个 fingerprint 都具有
compatibility eligibility。Object、digest、raw field 或 eligibility proof 缺失，或
任一 field/digest 不同，都会使受影响 B1/M1 的 candidate/reference throughput、
memory-reference 或其他 relative verdict 成为 `invalid`。若规范化 field 匹配且
各自 root-containment proof 成功，不同 disposable absolute path 仍可比较；相同
path string 绝不能覆盖 fingerprint mismatch。

每个 ordinal 为 `1..3` 的 M1 replicate 还要记录两个 same-subject pair：
`paired_isolated_i1={row_digest,bundle_digest,replicate_ordinal}` 与
`paired_isolated_b1_cap8={row_digest,bundle_digest,replicate_ordinal}`。Candidate M1
row 与 candidate isolated row 配对；reference M1 row 与 reference isolated row
配对。I1 pair 提供 relative latency denominator，B1 cap-8 pair 提供每个一秒
throughput denominator。二者都不能由 generic comparison reference 或一个含义
模糊的“reference bundle digest”替代。

Paired row 与 M1 row 必须具有相同 replicate ordinal、evidence schema version、
subject build/provider/plugin identity、worker count、resource limit/headroom、
cache/residency precondition 与 power/thermal eligibility policy。两个 isolated pair
都必须精确匹配 `base_environment_digest`。Paired I1 fixture hash 必须等于 M1
内嵌的 I1 component，但 I1 行保持
`storage_environment_applicability=not-applicable`；M1 的无关 storage field 不参与
I1 latency pair，也不会使其无效。Paired B1 fixture/corpus/golden hash 与 Run cap 8
必须等于 M1 B1 component，而且 M1/B1 pair 必须具有相同的完整
`environment-class digest` 与兼容 storage fingerprint。Pair 缺失、为零、ordinal
错误、跨 subject，存在 unknown/unobserved/unsupported/unprovable storage state，
或其他不兼容证据，都会使受影响 M1 relative verdict 成为 `invalid`。

所有被引用 bundle 与 row 都不可变，并按 content digest 选择。未记录的“known
good” build 重跑与 Markdown summary 都不是规范 reference。Raw evidence 必须能够
复算每个 aggregate 与 verdict。

### 后续 Issue 负责固定的证据行

| Issue | 必需 v1 交付 |
| --- | --- |
| #93 | 实现 I1 request/current-generation 与 cancellation/quiescence 观测；发布 isolated latency、waste 与 memory 行，以及必需的 output-correctness 证据。 |
| #94 | 在精确 I1 lineage 上实现 I2；发布 preview/final latency、Host/条件式 Metal residency 与 copy-waste、memory 行，以及必需的 output-correctness 证据。 |
| #95 | 实现 B1 immutable manifest、reservation、canonical semantic trace、crash-durable artifact commit、storage-environment collection/canonicalization 与 logical/raw golden；在 Run cap 1 与 8 下发布 isolated throughput、determinism、zero-fault waste 与 memory 行。 |
| #96 | 把精确 I1 与 B1 fixture 组合为 M1，记录其必需 storage fingerprint，强制执行 M1/B1 storage pair，同时让 I1-only pair 不依赖 storage，并发布 mixed latency、throughput progress、fairness、waste 与 memory 行。 |

每个 Issue 可以为其机制新增长期确定性行为测试，但不能重定义 workload，也不能
用缺失、invalid 或不同版本的行提升目标。与机器相关的 latency、throughput 与
reference ratio 保持为长期手工/release benchmark，不是普通 CTest 或默认 CI
correctness gate。

## 后果

- Interactive speed 不能掩盖 starvation、nondeterminism、过量 waste 或 memory
  overcommit；throughput 不能掩盖 latency failure。
- 绝对 Interactive budget 可能在有效但较慢的机器上失败。证据必须如实报告；
  改变 v1 需要 superseding decision，不能本机放宽。
- Relative gate 可能认可一个很慢的 reference，因此绝对 latency、精确 determinism
  与 resource ceiling 继续作为独立门禁。
- 可信 `work_units` 是估计值，不是 elapsed CPU time。它只用于已声明的
  scheduling-service unit；wall throughput 与 latency 保持独立。
- Telemetry gap 会使受影响证据无效，不允许外推。
- #92 有意不新增 placeholder field。没有来源的零值 SLO field 会显得具有权威性；
  在这里实现完整 collector 又会吞并 #93 至 #96 的独立交付范围。

后续消费开始后，改变本契约必须创建新的 workload 或 evidence-schema version；
若已接受决策发生变化，还必须建立 superseding ADR。既有证据保持不可变且可解释。
