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
| `I1-edit-storm-v1` | 使用 seed zero 与自然序号为 `1..12` 的十二次 edit。对 `0..11` 中的 `edit_index = edit_ordinal - 1`，第一个 node 的 `k` 从 `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]` 取值，source Region 为 `(256*(edit_index mod 4), 256*floor(edit_index/4), 256, 256)`。每个 Run 使用 `ComputeIntent::GlobalHighPrecision`、`ComputeRunQuality::Full`、Interactive QoS、weight 1、Run cap 8、经过 checked arithmetic 的 absolute monotonic deadline `D_i=A_i+150,000,000 ns`，以及精确的 `(Graph, target node four, GlobalHighPrecision)` supersession key。第十二次 edit（`edit_index=11`、`k=1.04`、Region `(768,512,256,256)`）是唯一必需 publication，且必须不晚于 `D_11` 发布。单一连续的 221-slot cold/warmup/measured grid 固定全部 isolated episode origin；每个 episode 的 500 ms settlement-observation window 从其第十二次 nominal start `S_11` 开始，并在下一 origin 前结束。 |
| `I2-progressive-v1` | 复用精确的 I1 source、graph、seed、edit ordinal、source-space Region、realtime request lineage，以及完整的第一个 node coefficient 序列 `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]`，并使用相同的 `edit_index=edit_ordinal-1` 映射。一个连续的 111-slot steady-clock grid 连接 cold、warmup 与 measured phase origin；episode 精确相隔 1,500,000,000 ns，并包含十二个相隔 16,666,667 ns、最多迟到 2,000,000 ns 的 nominal preview-admission start。每个 index 都先把第一个 node 更新为对应 coefficient，再按顺序执行 node one 至 node four，其 `k` 值为 `[coefficient, 1.00, 1.20, 1.40]`。512x512 preview source 是对原始 2048 source 逐 channel 执行 4x4 box average、只舍入一次到 binary32 后，再执行该 update/transform 序列；preview Region `edit_index` 为 `(64*(edit_index mod 4), 64*floor(edit_index/4), 64, 64)`。Final 从原始 2048 source 开始，使用与 I1 相同的 full-resolution update/transform path，绝不从 preview pixel 派生。只有第十二次 edit（`edit_index=11`、preview Region `(192,128,64,64)`）具有必需的 preview 与 final latency result，且必须按此顺序出现；stale output 不得发布。 |
| `B1-immutable-v1` | 包含 immutable job `0..29`；job `n` 使用 source seed `n`、baseline graph、Throughput QoS、weight 1、无 deadline 或 supersession、精确 reservation 证据、canonical semantic trace、crash-durable committed artifact 与按 job index 区分的 logical/raw golden。偶数 job 属于 Graph A，奇数 job 属于 Graph B。测量边界上 harness 同时提供两个有序的 15-job queue，且不会暂停非空 queue；由有界 Host admission 而非 harness 决定驻留多少个 Run。Run cap 1 与 8 是两行独立的必需证据。 |
| `M1-shared-v1` | 从测量边界精确派生 cold/warmup boundary `C^M1=B^M1-6,000,000,000 ns` 与 `W^M1=B^M1-5,000,000,000 ns`，先运行一个 cold I1 origin 和固定 B1 seed-252 job，再运行七个 warmup I1 origin 与固定 seed-253/254/255 B1 protocol。在精确 warmup-cutoff/measurement-origin boundary 启动 measured I1，此后每 750,000,000 ns 重复一次，共精确启动 40 个 episode。偶数 Graph A 与奇数 Graph B producer 随后各自在独立 producer-local cycle 中重复自己的 15-job 子序列，保持 Run cap 8 与持续 offered backlog，共测量 30 秒；任一 producer 都不等待另一个完成相同 local ordinal。该 boundary 既不暂停也不排空 shared domain：已经 offered 的 warmup work 会保留其 phase identity 与 resource authority，并位于新 offered 的 measured B1 work 之前。两条 stream 共用一个 `ExecutionService`、worker set、ready store、policy binding set 与 `ResourceLedger`；不得用隐藏 pool、重复 ledger 或独立进程承接任一 stream。 |

每个携带 workload 的 field 或 fixed-record component 都使用专用、区分大小写的
scalar type `workload-id-v1`。其完整 domain 精确且仅包含
`I1-edit-storm-v1`、`I2-progressive-v1`、`B1-immutable-v1` 与
`M1-shared-v1`；它不执行 case folding、alias、Unicode normalization，也不接受开放式
identifier。上述 raw ASCII payload 的精确 frame 分别为
`16:I1-edit-storm-v1`、`17:I2-progressive-v1`、
`15:B1-immutable-v1` 与 `12:M1-shared-v1`。通用 `identifier` type 继续保持
lowercase-only，并继续用于所有声明为该类型的非 workload field。

对 fairness 而言，只要某个 Graph 的 producer 仍有未消费的 offered demand 且没有
暂停提交，该 Graph 就是 *eligible*。这段 workload-level interval 包含等待有界
admission 的时间；它不声明全部 30 个 B1 Run 同时被准入。每个 Graph 内部按递增
job index 提供工作，前一个 job terminal 时 producer 同步提供下一个。Measured M1
中，Graph A 重复 `0,2,...,28`，Graph B 重复 `1,3,...,29`。每个 producer 在自己的
最后一个 job terminal 后立即开始自己的下一轮 15-job local cycle；较快 producer
可以已经位于 local cycle `c+1`，而另一个仍在 `c`。共享 cross-Graph cycle barrier
或等待另一个 producer 形成的 gap 都是 invalid。

#### B1 job occurrence identity 与 retry identity 相互独立

`job_index` 继续作为 `0..255` 中不可变的 fixture 与 golden selector；由于 M1 会在
每个 cycle 复用 `0..29`，它不足以标识一次 execution occurrence。因此，每个包含
B1 的 cold、warmup 或 measured row 都要为每个 offered job 分配一个 canonical
`job-instance-v1` fixed record，其 component 按以下精确顺序排列：

```text
(row_workload_id:workload-id-v1,
 replicate_ordinal:uint64,
 phase:enum(cold|warmup|measured),
 cycle_ordinal:uint64,
 job_index:uint64,
 run_cap:uint64)
```

Canonical payload 按下文 fixed-record grammar，为每个 component payload 依次拼接
一个 `frame(component-payload)`。`replicate_ordinal` 为 `1..3`；`job_index` 为
`0..255`；`run_cap` 是该 row 冻结的 cap。每个 phase 中的 `cycle_ordinal` 都从零
开始。B1 cold/warmup seed job 和 isolated measured job 使用 cycle zero。对于
measured M1，未改变的 wire component 存储 `producer_cycle_ordinal`；producer lane
无需新增 field，而是由偶数 Graph A 与奇数 Graph B `job_index` 推导。Graph A 只在
当前 local cycle 的 job 28 terminal 后递增自己的 counter，Graph B 则独立地只在
job 29 terminal 后递增；二者随后分别立即 offer 新 local cycle 的 job zero 或 one。
任一 producer 都不等待、递增或完成另一个 producer 的 cycle。同一个包含 B1 的 row
中，`(phase,cycle_ordinal,job_index)` 仍不得重复；既有六 component record、outer
schema 与 retry 语义均不改变。

Logical Compute I/O task 是 `(job_instance_id,stage)`，其中 `stage` 为
`payload-stage` 或 `manifest-commit`；完整 attempt identity 为
`(job_instance_id,stage,attempt)`。`attempt` 从零开始，且只有显式 retry/
reconciliation policy 在 terminal failure 后重新签发同一个 logical task 时才改变。
Capacity rejection、重复 observation 或幂等 duplicate `try_submit` 保持相同 attempt
identity 与 charge。`cycle_ordinal` 绝不能编码为 `attempt`、从 `attempt` 推断，也
不能令其递增。Fault-free B1/M1 只允许 attempt zero，且每个 logical task 只接受一次
admission 并启动一次。

B1 work 的每个 charge declaration、admission/status event、ledger 或 executor
snapshot、start/terminal record、`OutputCommitId`、rooted no-replace output slot、
`OutputCommitReceipt` 和 row-evidence entry 都绑定完整 `job_instance_id`；task-specific
record 还要绑定 `stage` 与 `attempt`。不同 cycle 可以具有相同 fixture/golden 与
semantic-trace digest，但必须具有不同 commit identity、output slot、receipt 与 evidence
key。Normalized semantic trace 刻意继续编码 `job_index`，而不是 occurrence 或 physical
scheduling identity，以便进行精确 determinism comparison；row 中的 job-instance index
把每份 retained physical trace 及 trace digest 绑定到唯一 occurrence。

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

Harness 禁止在 `S_i` 之前启动 Host admission call。`A_i` 是最终 Host admission
invocation 之前立即捕获的唯一 monotonic-clock sample；它同时作为 latency start 与
deadline anchor。Harness 在调用 Host 前检查
`S_i <= A_i <= S_i + 2,000,000 ns`，并用 checked arithmetic 计算唯一 absolute Run
deadline：

```text
D_i = A_i + 150,000,000 ns
```

把 `D_i` 锚定到 `S_i`、episode origin、更早的 preparation timestamp 或
post-admission return time 都无效。允许的 start lateness 不会消耗 150 ms Run budget。
Overflow、提前启动、迟于 2 ms 启动、admission failure、dropped edit 或 cadence-event
gap 都会使 replicate 无效。Missed edit 不会迟到补交：在为该 edit 调用任何 Host
边界前，harness 请求此前全部 generation 的 cancellation/supersession、记录其被接受，
撤销整个 episode 的 publication permission，记录 missed/drop/gap 事实，并且绝不
追赶、回填或移动后续名义时刻。已经进入的 non-preemptible work 可以 drain，并按
waste 计费；accepted cancellation 之后启动的 work 必须保持为零。任何无效或
expired edit 都不得发布 output、receipt 或 successful latency sample。

在 `D_i` 过期时使用相同 monotonic clock，请求该 Run 的 cancellation 并记录其被接受。
Queued work 会被移除，dependent re-entry 会被拒绝，已经进入的 non-preemptible work
会在没有 commit authority 的情况下 drain。即使 execution 后来成功，
deadline-expired result 也不能成为 current。这些规则适用于每个 isolated 与 M1 I1
episode，包括第十二次 edit。Settlement-observation window 使用名义上的第十二次
start 作为独立 anchor，而不是可变的 admission 或 deadline：

```text
Q^I1_start(E) = S_11 = E + 11 * 16,666,667 ns
                = E + 183,333,337 ns
Q^I1_end(E) = Q^I1_start(E) + 500,000,000 ns
              = E + 683,333,337 ns
```

该 window 包含两个 boundary 上的事件。在 `Q^I1_start`，nominal schedule marker
先于同 timestamp 的 actual admission 排序。在 `Q^I1_end`，所有 timestamp 相等的
lifecycle event 会先按保留的 causal order 应用，随后才取得 quiescence snapshot。
如果某次 start 使该 snapshot 仍有 active work，或任一 terminal/settlement event
位于 boundary 之后，则 replicate 无效。该 window 可以观察仍 active 的 final Run；
它不会取消 work、延迟下一 origin 或延长任何 `D_i`。在最晚合法 admission 下，
`D_11 <= E + 335,333,337 ns`，因此从该 deadline 到 `Q^I1_end` 精确保留
348,000,000 ns，从 `Q^I1_end` 到下一 750,000,000 ns origin 精确保留
66,666,663 ns。Reset/baseline preparation 必须使用这段固定剩余 guard，并在下一
origin 前完成，不能移动 origin。每项 grid、nominal-start、admission、deadline 与
drain 计算都使用 checked arithmetic；overflow 使结果无效。

一个保留的 isolated-I1 replicate-grid origin `G^I1` 固定全部 phase，不允许使用
三个彼此独立的 origin：

```text
E^I1_g = G^I1 + g * 750,000,000 ns
E^I1_cold,0 = E^I1_0
E^I1_warmup,r = E^I1_(1+r),       r in 0..19
E^I1_measured,r = E^I1_(21+r),    r in 0..199
T^I1 = G^I1 + 221 * 750,000,000 ns
```

Natural episode ordinal 在所属 phase 内映射为从零开始的 `r`。Cold 占 slot zero，
warmup 占 slot `1..20`，measured 占 slot `21..220`；`T^I1` 是 terminal
non-start boundary。Counter reset 在已固定的 measured origin 前完成。任何 phase
都不得另选 origin、插入 cooling delay 或移动后续 slot。每个 episode 必须在自己的
`Q^I1_end` 达到 quiescent；因此最后一个 measured episode 也必须在 `T^I1` 前，
以同样精确的 66,666,663 ns guard 完成 settlement。

M1 单独对 `r=0..39` 使用 `E_r = M_0 + r * 750,000,000 ns`，其中 `M_0`
是下文定义的精确 mixed-load warmup cutoff 与 measurement origin。因此，配对的
isolated 与 mixed 证据共享相同的逐 episode schedule、start-lateness、drain 与
miss/drop/gap 规则，但不声称具有相同 physical wake time。

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

I2 使用同一个 steady-clock domain，但冻结自身完整的 episode cadence。在 cold、
warmup 或 measured phase 各自的局部 enumeration 内，自然序号
`episode_ordinal=1..N` 映射为从零开始的
`episode_index=r=episode_ordinal-1`；measured I2 精确使用 `N=100` 与 `r=0..99`。
一个保留在证据中的 phase origin `E^I2_0` 按下式固定所有 origin：

```text
E^I2_r = E^I2_0 + r * 1,500,000,000 ns
S^I2_{r,i} = E^I2_r + i * 16,666,667 ns,  i in 0..11
```

这里的 `E^I2_0` 是当前 phase 的第一个 origin。一个保留在证据中的 replicate-grid
origin `G^I2` 会在没有 pause 的情况下精确确定三个 phase origin：

```text
E^I2_{cold,0} = G^I2
E^I2_{warmup,0} = G^I2 + 1 * 1,500,000,000 ns
E^I2_{measured,0} = G^I2 + 11 * 1,500,000,000 ns
T^I2 = G^I2 + 111 * 1,500,000,000 ns
```

因此，cold、warmup 与 measured episode start 位于一个连续的 111-slot grid 上。
Warmup-to-measured counter reset 必须在已经固定的 measured origin 前完成；它绝不
插入 cooling delay，也不重新选择 clock origin。`T^I2` 是 terminal grid boundary，
不会启动 episode。

Harness 在 `S^I2_{r,i}` 之前准备 immutable edit input。`A^I2_{r,i}` 是 preview Host
admission 前立即捕获的唯一 monotonic sample，也是规范的 edit acceptance/admission
时刻：成功调用会在一个有序边界中接受 edit、令预先生成的 generation 成为 current，
并 admit preview child。调用前，harness 必须证明
`S^I2_{r,i} <= A^I2_{r,i} <= S^I2_{r,i} + 2,000,000 ns`，并通过 checked addition
得到且保留两个 absolute child deadline：

```text
D^preview_{r,i} = A^I2_{r,i} + 100,000,000 ns
D^final_{r,i} = A^I2_{r,i} + 1,000,000,000 ns
```

Overflow、提前 admission、admission 迟到超过 2 ms、admission failure、edit
缺失/重复/失序或 cadence-event gap 都会使 replicate 无效。缺失或无效的 edit 绝不
迟到补交；publication 会被撤销，accepted cancellation/supersession 会被记录，且
任何后续 nominal start 或 episode origin 都不得为了追赶、回填或降温而移动。已经
进入的 work 只能作为 waste drain；accepted cancellation 之后启动的 work 保持为零。
Baseline preparation 与前一个 episode 的全部 work 必须在下一个固定 origin 前
quiescent；最后一个 measured episode 必须在 `T^I2` 前 quiescent。失败意味着无效，
不是移动任一 boundary 的许可。

1,500,000,000 ns stride 是精确契约，而非 runner 自选 pacing。即使第十二次 admission
达到最晚合法时刻，也有
`D^final_{r,11} <= E^I2_r + 1,185,333,337 ns`，从而在 `E^I2_{r+1}` 前留下精确的
最小 314,666,663 ns quiescence guard；对最后一个 measured episode，该 guard 位于
`T^I2` 前。该 guard 不会延长 final deadline。因此，一个 cold、十个 warmup 与 100 个
measured episode 使用同一个连续 replicate grid；只有
100 个 measured episode 的第十二次 preview/final pair 进入
steady-state aggregate。十个 warmup slot 与 100 个 measured slot 的 nominal phase
span 分别是 15 s 与 150 s，与 I1 的 20×750-ms warmup 与 200×750-ms measured pacing
相同，同时不与 I2 的 1,000 ms final deadline 重叠。

对于该 state machine，“相同 I1 lineage”还冻结完整 numeric 与 execution sequence。令
`K=[0.82,1.18,0.86,1.14,0.90,1.10,0.94,1.06,0.98,1.02,0.96,1.04]`。
对于 `0..11` 中每个 `edit_index=i`，I2 使用 `K[i]`、index `i` 对应的 I1 source
Region、index `i` 对应的 I2 preview Region，以及相同 ordinal/generation record。每个
preview 都先从原始 2048 source 逐 channel 计算 4x4 box average，并把该 source 只
舍入一次到 binary32；然后把第一个 node 更新为 `K[i]`，再依次执行 node one、node
two、node three 与 node four，其 `k` 值为 `[K[i],1.00,1.20,1.40]`。每个 final 则
从原始 2048 source 开始，执行相同的第一个 node update，以及与 I1 相同的四 node
full-resolution path。Final 不得 upsample、复用或以其他方式从 preview pixel 派生。
替换 coefficient、重排 sequence、偏移 index、令 Region 与 index 不匹配、改变 rounding
point 或改变 final path，都不属于 `I2-progressive-v1`：携带该 id 的 row 无效，而
有意改变的 fixture 必须使用新的 workload id。

Final child 只在该 edit 的 preview 首次可见、且其 generation 仍为 current 时精确
提交。Edit `0..10` 不会等待其 preview：`i+1` 的 acceptance 仍由
`S^I2_{r,i+1}` 与 `A^I2_{r,i+1}` 固定。只有 preview `i` 的 visibility timestamp
严格早于 `A^I2_{r,i+1}` 时，它才仍为 current；二者相等时，更新 edit 的 acceptance
先排序，该 preview 属于 stale。若更新的 edit 先被接受，armed final 不经提交即被丢弃；已经提交的 preview
或 final 被 supersede，且只能 drain。新 generation 会撤销两个较旧 child 的
publication permission。Stale terminal event 可以更新 cleanup、waste 与 quiescence
证据，但不能发布 `Value`、digest、receipt 或必需 latency result。第十二次 edit
（`edit_index=11`）必须先发布 preview、再发布 final；failure、deadline expiry、
反序、重复 publication，或在两个 endpoint 完成前出现更新 generation，都会使
episode 无效。较早 generation 只能在仍为 current 时发布，并不属于必需 result。
第十二次 preview 必须不晚于 `D^preview_{r,11}` 可见；其 final 必须不晚于
`D^final_{r,11}` 可见。Expiry 使用同一个 clock，撤销 publication，并且绝不重新锚定
任一 deadline。

Preview latency 从 preview Host admission call 前的 `A^I2_{r,i}` 立即开始，到 current preview
可见时结束。Final end-to-end latency 使用同一起点，到 current final 可见时结束；
较晚的 final trigger 与 Host admission timestamp 单独保留，但绝不重置
`D^final_{r,i}`。因此 final 门禁包含 preview、trigger、admission、execution 与
publication，不会隐藏 preview interval。

本 cadence 不改变 outer evidence envelope 的 field。既有
`execution-profile-workload-manifest-v1` section 保留 clock domain、replicate-grid
origin、派生 phase origin、terminal boundary、episode ordinal/index、stride、十二个
nominal start、lateness bound、deadline formula 与同 timestamp ordering rule。既有
`execution-profile-measurement-evidence-v1`
section 与 raw event 保留每个 `E^I2`、`S^I2`、`A^I2`、child deadline、preview
visibility、final trigger/admission/visibility、cancellation、gap/drop 与 quiescence
observation。其既有 section digest 与 verdict evidence 足以支持独立 cadence oracle；
封闭的 15-record row 与五 record bundle 不新增 field。Origin/index、episode stride、
edit cadence/order、start-lateness、deadline anchor 或 equal-time ordering 任一漂移，
都会使带 `I2-progressive-v1` 标签的 row 无效。有意改变时必须使用新的 workload id
和新的 manifest/digest/golden lineage。

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
golden。Workload manifest 与 fixture oracle 绑定完整 `K` array、index/Region mapping、
node update/transform order、preview average-and-rounding order，以及 full-resolution
final path；任一输入漂移都会改变 manifest，不能在 v1 digest/golden linkage 下被接受。

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
task，稳定 charge identity 分别为
`(job_instance_id, payload-stage, attempt)` 与
`(job_instance_id, manifest-commit, attempt)`。Payload-stage task 声明
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
output slot、完整 `job_instance_id`、job index、descriptor 与 logical content
identity、committed version/
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

#### Storage、Base 与逐行 Environment Manifest 是封闭的 v1 Schema

本 ADR 是规范字节 schema 的权威来源。v1 producer 不能新增 extension field、用
provider 特有 object 替代固定对象、遗漏固定 field，也不能把“等价”解释为可以另造
一种表示。Provider adapter 要么把 observation 映射到下列精确 field，要么发出
ineligible state。

三个 manifest 都是 ASCII，并使用同一 field-record grammar。对 ASCII byte string
`B`，`frame(B)` 是不补零的十进制 byte length、一个冒号与 `B`。因此空 frame 是
`0:`。一个 field record 精确为：

```text
field=<frame(name)><frame(state)><frame(reason)><frame(type)><frame(payload)>\n
```

字面 header、每个 field record 与最后一个 LF 都属于 digest 输入。不得出现 BOM、
CR、缩进、trailing space、依赖 locale 的格式或末尾额外空行。下列表格固定每条
record 的顺序、type 与 cardinality。缺失、重复、重排、field name 未知或额外的
record 都不属于 v1 manifest。

Scalar 与 composite payload grammar 是封闭的：

- `identifier` 是匹配 `[a-z0-9][a-z0-9._+-]*` 的非空小写 ASCII。
  `workload-id-v1` 是上文精确四值 domain 中的一个 raw ASCII token，并区分大小写；
  它不是 `identifier`，lowercase alias 或未知 token 都是 invalid。`enum` 是 field
  特有集合中的一个精确 token。
- `uint64` 是完整匹配 `0|[1-9][0-9]*` 的 ASCII 十进制数，解释后的取值闭区间为
  `0..18446744073709551615`；`00`、`01`、任何其他前导零拼写以及溢出均为
  invalid。Field table 可以规定更高下限。`boolean` 精确为 `true` 或 `false`，
  `sha256` 精确为 64 个小写十六进制 digit。
- `text` 是非空有效 UTF-8；先规范化为 Unicode NFC，再把每个 UTF-8 byte 编码为
  两个小写十六进制 digit。这样 manifest 保持 ASCII，同时保留区分大小写的 identity
  byte。
- 通用 list payload 是
  `<unpadded-count>:<frame(item-1)>...<frame(item-n)>`。通用 map payload 是
  `<unpadded-count>:<frame(key-1)><frame(value-1)>...`。通用 fixed-record payload
  按已声明顺序，拼接每个 component 的 canonical scalar 或 composite payload 的一个
  frame；component name 与 type token 是 schema metadata，不出现在该 payload 中。
  Frame length 与 collection count 使用 `uint64` lexical form 与 range；list count
  是 item 数，map count 是 key/value pair 数。Checked parsing 拒绝 arithmetic
  overflow、超出剩余 input 的声明 length，或未精确消费相应数量 value 的 count。
- `token-set-v1` 精确绑定通用 list grammar；item 是 field 封闭 enum/identifier
  universe 中一个精确 token 的 raw ASCII byte。Item 按未加 frame 的 token byte 进行
  unsigned ASCII 排序并保持唯一。Encoded payload 是 count 后接每个 token 的一个
  frame；空 set 精确为 `0:`。Duplicate 或不属于 field 封闭 universe 的 token 为
  invalid，而不仅是 ineligible。
- `ordered-text-list-v1` 精确绑定通用 list grammar；item byte 是一个 `text` value 的
  canonical lowercase-hex payload，不是完整 field record，也不额外包含 text frame；
  list grammar 提供唯一一层 item frame。Item 保留 compiler invocation order 并允许
  重复；空 list 精确为 `0:`。
- `cpu-record-list-v1`、`device-record-list-v1` 与
  `contract-record-list-v1` 精确绑定通用 list grammar。每个 item 是包裹完整对应
  fixed-record payload 的一个 frame。排序与 duplicate detection 对未加外层 frame 的
  完整 fixed-record payload byte 执行 unsigned ASCII 比较，而不是只比较
  `stable_identity` 或 `contract_id`。CPU 与 provider-contract list 至少一项；GPU、
  other-device 与 plugin-contract list 可以精确为 `0:`。
- `mount-map-v1` 与 `commit-semantics-v1` 精确绑定通用 map grammar。Key 与 value
  item byte 是下文展示的 raw ASCII token，分别只由 map grammar 包裹一层 frame。
  Key 按未加 frame 的 unsigned ASCII byte 排序且保持唯一；count 分别精确为七个与
  六个 key/value pair。
- `cpu-record-v1`、`device-record-v1`、`contract-record-v1`、
  `b1-performance-configuration-v1`、`resource-limits-v1`、
  `metal-resource-limits-v1`、`cache-preconditions-v1`、
  `residency-preconditions-v1`、`power-policy-v1` 与
  `thermal-eligibility-v1` 精确绑定通用 fixed-record grammar。其 component order 与
  scalar type 在下文固定；不存在 separator、component-name text、遗漏 component 或
  替代 provider object。
- 即使 `state` 不是 `known`，`type` frame 仍包含 field table 中的精确 type token。
  `known` record 的 reason 是 `none`，payload 是规范的非空 payload（允许为空的
  collection 使用显式 payload `0:`）。其他 state 的 payload 都是零 byte，由最后的
  `0:` frame 编码。

例如，完整的 known I1 workload field record 为：

```text
field=11:workload_id5:known4:none14:workload-id-v116:I1-edit-storm-v1\n
```

其余三个 payload frame 是上文列出的精确 17、15 与 12 byte frame。
`job-instance-v1` 或 `row-reference-v1` fixed-record payload 只包含 component payload
frame，因此本次纠错保留这些 token byte，只改变其 schema/parser domain。相反，
evidence row 与 bundle field record 的 canonical byte 包含 type frame：必须使用
`14:workload-id-v1`，`10:identifier` 为 invalid，而且每个 row 或 bundle address
都必须按纠正后的 byte 重新计算。此前的 `identifier` annotation 在未改变的
lowercase grammar 下无法编码四个 uppercase-leading workload，因此没有定义可供
重新解释的有效 legacy v1 object。

八项 durability capability 的示例是逐 byte 精确的。其 `token-set-v1` payload
包含 156 个 ASCII byte：

```text
8:17:atomic-no-replace14:atomic-visible13:crash-durable20:idempotent-reconcile13:manifest-last13:manifest-sync28:namespace-durability-barrier12:payload-sync
```

完整 field record（包括最后一个 LF）为 221 byte：

```text
field=23:durability_capabilities5:known4:none12:token-set-v1156:8:17:atomic-no-replace14:atomic-visible13:crash-durable20:idempotent-reconcile13:manifest-last13:manifest-sync28:namespace-durability-barrier12:payload-sync\n
```

在这里及下文 inline byte 示例中，显示的末尾 `\n` 表示一个 LF byte。Known empty
collection 与 non-applicable scalar 具有不同 byte。Known-empty `build_flags` 会为
两 byte list payload `0:` 加 frame，因此结尾是 `2:0:`：

```text
field=11:build_flags5:known4:none20:ordered-text-list-v12:0:\n
```

相比之下，I1/I2 的 non-applicable storage digest 具有零 byte payload，因此结尾是
`0:`：

```text
field=26:storage_environment_digest14:not-applicable24:row-has-no-output-commit6:sha2560:\n
```

State 与 reason 同样封闭：

| State | 允许的精确 reason | Compatibility eligibility |
| --- | --- | --- |
| `known` | `none` | Value 与全部 cross-field rule 通过时 eligible。 |
| `not-applicable` | 下表中一个 field 特有 reason。 | 只有保留证据证明命名 layer 不存在于完整 durability/execution path 中时才 eligible。 |
| `unknown` | `probe-returned-indeterminate` | 永不 eligible。 |
| `unobserved` | `probe-not-run` 或 `probe-failed-before-observation` | 永不 eligible。 |
| `unsupported` | `probe-contract-unsupported` 或 `platform-capability-unsupported` | 永不 eligible。 |
| `unprovable` | `evidence-chain-incomplete` 或 `conflicting-effective-values` | 永不 eligible。 |

v1 唯一允许的 `not-applicable` pair 是：

| Field | 精确 reason |
| --- | --- |
| storage `filesystem_type` | `filesystem-layer-absent` |
| storage `mount_identity`、`mount_effective_options` | `mount-layer-absent` |
| storage `hardware_write_cache_policy` | `hardware-write-cache-layer-absent` |
| storage `power_loss_protection_policy` | `power-loss-protection-layer-absent` |
| base `metal_resource_limits` | `configured-metal-executor-absent` |
| environment-class `storage_environment_digest` | `row-has-no-output-commit` |

三个 environment manifest 中的其他 field 都不接受 `not-applicable`。下文的
evidence-row 与 bundle envelope 会定义自身封闭的 optional-reference reason。Layer
不透明、缺少 probe 或远端 provider boundary 不等于 layer 缺失；这些情况使用四种
ineligible state 之一。

每个 B1 或 M1 行都设置 `storage_environment_applicability=required`。其 storage
manifest 以精确 header `execution-profile-storage-environment-v1\n` 开始，随后精确
包含以下 21 条 record：

| # | Field | 精确 type 与 known value domain | 允许的 N/A |
| ---: | --- | --- | --- |
| 1 | `output_store_contract_id` | `identifier`；稳定 `OutputStore` contract id | 否 |
| 2 | `output_store_contract_generation` | `uint64`；`1..18446744073709551615` | 否 |
| 3 | `backend_semantics_id` | `identifier`；稳定 normalization/semantics contract id | 否 |
| 4 | `backend_semantics_generation` | `uint64`；`1..18446744073709551615` | 否 |
| 5 | `backend_instance_id` | `text`；稳定 account/export/bucket/filesystem instance，不含 disposable job directory | 否 |
| 6 | `backend_class` | `enum`：`filesystem`、`network-filesystem`、`object-store`、`memory-store` 或 `composite` | 否 |
| 7 | `locality` | `enum`：`process-local`、`host-local` 或 `network-remote` | 否 |
| 8 | `persistence` | `enum`：`volatile`、`host-restart-persistent` 或 `externally-persistent` | 否 |
| 9 | `filesystem_type` | `identifier`；规范化 filesystem type | `filesystem-layer-absent` |
| 10 | `mount_identity` | `text`；稳定 filesystem/export mount identity，不是 path spelling | `mount-layer-absent` |
| 11 | `mount_effective_options` | `mount-map-v1`；下述精确 map | `mount-layer-absent` |
| 12 | `commit_semantics` | `commit-semantics-v1`；下述精确 map，包含 provider transaction equivalent | 否 |
| 13 | `durability_capabilities` | `token-set-v1`；下述封闭 capability set 的 subset | 否 |
| 14 | `requested_durability` | `enum`：`atomic-visible` 或 `crash-durable` | 否 |
| 15 | `achieved_durability` | `enum`：`atomic-visible` 或 `crash-durable` | 否 |
| 16 | `durability_endpoint_identity` | `text`；最后一个必需 barrier 或 provider commit 延伸到的已配置 namespace/root | 否 |
| 17 | `durability_anchor_identity` | `text`；稳定 backing filesystem、volume、device、bucket 或 provider durability-domain identity | 否 |
| 18 | `storage_class` | `enum`：`memory`、`local-block`、`remote-block`、`network-filesystem`、`object` 或 `composite` | 否 |
| 19 | `b1_performance_configuration` | `b1-performance-configuration-v1`；下述精确 fixed record | 否 |
| 20 | `hardware_write_cache_policy` | `enum`：`disabled`、`write-through`、`write-back-protected`、`write-back-unprotected`、`provider-managed-protected` 或 `provider-managed-unprotected` | `hardware-write-cache-layer-absent` |
| 21 | `power_loss_protection_policy` | `enum`：`present`、`absent`、`provider-guaranteed` 或 `provider-not-guaranteed` | `power-loss-protection-layer-absent` |

`durability_anchor_identity` 是此前被描述为 provider 特有 equivalent 的 backing
volume/device/storage identity 的唯一固定表示。Adapter 不新增替代 field。所选 absolute
root、解析后的 path component、fresh job directory 与 root-containment proof 保持为
manifest 外部的 raw audit evidence；它们不能替代稳定 instance、endpoint、mount 或
anchor identity。

`mount-map-v1` 精确包含下列七个小写 key/value pair，并按表中顺序编码；这些 enum
以外的 key 或 value 都无效：

| Key | 精确 value enum |
| --- | --- |
| `access_mode` | `read-only`、`read-write` |
| `atime_policy` | `strict`、`relaxed`、`none` |
| `cache_coherence` | `host-local`、`close-to-open`、`strong`、`eventual` |
| `copy_on_write_mode` | `disabled`、`enabled`、`provider-managed` |
| `data_write_mode` | `buffered`、`synchronous` |
| `journal_mode` | `none`、`writeback`、`ordered`、`full`、`provider-managed` |
| `metadata_write_mode` | `buffered`、`synchronous` |

Collector 解析的是 effective behavior，不是输入 spelling。省略 native option 与显式
指定 default 时发出相同 value。只有在 platform contract 声明 option domain 对 ASCII
大小写不敏感时才折叠 native case；canonical key 与 enum value 始终是上述小写 token。
Canonical map 不保留 native order 或 duplicate。如果 platform 定义了确定的 duplicate
winner，collector 要 probe 并发出那一个 effective value；否则冲突 duplicate 产生
`unprovable/conflicting-effective-values`。只有保留的证据能够证明未知 native option
不会影响七个 key、`commit_semantics`、固定 B1 performance configuration、hardware-
cache/PLP policy 或任何 measured storage-path timing 时，才可将其排除；否则相关
record 为 `unprovable/evidence-chain-incomplete`。缺少 canonical key、额外 key、
duplicate key、未排序 key 或 raw/canonical 不一致均为 invalid。

`commit-semantics-v1` 是下列六项 map，并按表中顺序编码。它适用于全部 backend，
包括 known value 是 provider transaction 而不是 filesystem primitive 的 backend：

| Key | 精确 value enum |
| --- | --- |
| `atomic_no_replace` | `rename-no-replace`、`link-no-replace`、`conditional-create`、`provider-transaction` |
| `barrier` | `file-then-leaf-to-root`、`write-through`、`provider-transaction` |
| `copy_on_write` | `none`、`filesystem`、`backend` |
| `directory_sync` | `directory-fsync`、`full-fsync`、`write-through`、`provider-transaction` |
| `file_sync` | `file-fsync`、`full-fsync`、`write-through`、`provider-transaction` |
| `rename` | `same-namespace-atomic`、`conditional-rebind`、`provider-transaction` |

封闭的 `durability_capabilities` token universe 是 `atomic-no-replace`、
`atomic-visible`、`crash-durable`、`idempotent-reconcile`、`manifest-last`、
`manifest-sync`、`namespace-durability-barrier` 与 `payload-sync`。具备
compatibility eligibility 的 manifest 包含全部八项。Ineligible manifest 仍保留该 set，
使 validator 能精确报告缺少了什么。

`b1-performance-configuration-v1` 是一个 fixed-record payload，精确包含下列 37 个
component。每行按此顺序贡献一个包含 component canonical scalar payload 的 frame；
component name 不出现在 wire 中：

| # | Component | 精确 scalar type 与 domain |
| ---: | --- | --- |
| 1 | `compression_mode` | `enum`：`disabled`、`enabled` 或 `provider-managed` |
| 2 | `compression_algorithm` | `identifier`；精确规范化 algorithm id 或下述必需 sentinel |
| 3 | `compression_level` | `uint64`；effective numeric level，或下述精确 zero case |
| 4 | `compression_profile` | `identifier`；精确规范化 profile id 或 sentinel |
| 5 | `encryption_path` | `enum`：`none`、`host-client`、`filesystem`、`block-device`、`network-service`、`provider-managed` 或 `composite` |
| 6 | `encryption_profile` | `identifier`；精确 algorithm/mode/offload profile 或 `none` |
| 7 | `checksum_mode` | `enum`：`disabled`、`metadata-only`、`data-only`、`data-and-metadata` 或 `provider-managed` |
| 8 | `checksum_algorithm` | `identifier`；精确 storage checksum id 或 `none` |
| 9 | `deduplication_mode` | `enum`：`disabled`、`inline`、`post-process` 或 `provider-managed` |
| 10 | `logical_block_bytes` | `uint64`；effective logical block size |
| 11 | `physical_block_bytes` | `uint64`；effective physical block size |
| 12 | `record_bytes` | `uint64`；effective backend record/object-write unit |
| 13 | `allocation_unit_bytes` | `uint64`；effective allocation unit |
| 14 | `allocation_mode` | `enum`：`preallocated`、`on-demand`、`sparse`、`copy-on-write`、`memory-resident` 或 `provider-managed` |
| 15 | `provisioning_mode` | `enum`：`thick`、`thin`、`elastic`、`memory-resident` 或 `provider-managed` |
| 16 | `layout_mode` | `enum`：`single`、`striped`、`mirrored`、`replicated`、`erasure-coded` 或 `provider-managed` |
| 17 | `layout_data_units` | `uint64`；effective data-unit count |
| 18 | `layout_parity_units` | `uint64`；effective parity-unit count |
| 19 | `layout_replica_count` | `uint64`；effective complete-copy count |
| 20 | `layout_stripe_unit_bytes` | `uint64`；effective stripe/chunk unit |
| 21 | `layout_profile` | `identifier`；精确规范化 geometry/service profile 或 `none` |
| 22 | `upper_write_cache_mode` | `enum`：`absent`、`disabled`、`write-through`、`write-back` 或 `provider-managed` |
| 23 | `upper_write_cache_profile` | `identifier`；精确 filesystem/backend/provider cache profile、`none` 或 `not-applicable` |
| 24 | `io_scheduler` | `identifier`；精确规范化 scheduler/profile id |
| 25 | `io_queue_policy` | `enum`：`serial`、`fixed`、`unbounded` 或 `provider-managed` |
| 26 | `io_queue_depth` | `uint64`；符合下述规则的 effective queue depth |
| 27 | `io_concurrency_policy` | `enum`：`serial`、`fixed`、`unbounded` 或 `provider-managed` |
| 28 | `io_concurrency_limit` | `uint64`；符合下述规则的 effective storage-write concurrency |
| 29 | `network_path` | `enum`：`not-applicable`、`host-loopback`、`lan`、`wan` 或 `provider-internal` |
| 30 | `network_protocol` | `identifier`；精确 protocol/version profile 或 `not-applicable` |
| 31 | `network_link_profile` | `identifier`；精确 link/transport profile 或 `not-applicable` |
| 32 | `network_mtu_bytes` | `uint64`；effective path MTU；仅在不存在 network hop 时为零 |
| 33 | `network_qos_profile` | `identifier`；精确 QoS/traffic-class profile 或 `not-applicable` |
| 34 | `network_region` | `identifier`；精确 provider/placement region 或 `not-applicable` |
| 35 | `backend_service` | `identifier`；精确 backend/provider service id |
| 36 | `backend_performance_tier` | `identifier`；精确 service/performance tier 或 `not-applicable` |
| 37 | `device_performance_profile` | `identifier`；精确 device/volume performance profile 或 `not-applicable` |

该 fixed record 使用以下封闭 sentinel 与 cross-component rule：

- `compression_mode=disabled` 要求 algorithm/profile 为 `none`，level 为零。
  `enabled` 要求非 `none` algorithm，以及命名精确 effective default 或 explicit
  parameter set 的 profile；`provider-managed` 要求 algorithm 为
  `provider-managed`、level 为零且具有稳定的非 `none` profile。除非 named profile
  在已记录 backend-semantics generation 下定义该精确 default，否则 default level
  不能被静默编码为零。
- `encryption_path=none` 要求 `encryption_profile=none`；其他 path 要求非 `none`
  profile，在不记录 secret 或 key material 的前提下标识 algorithm、mode 与 offload
  path。`checksum_mode=disabled` 要求 `checksum_algorithm=none`；其他 checksum mode
  要求精确 algorithm id。Deduplication 没有 omitted state：`disabled` 是显式
  known-disabled value。
- Byte-unit component 只有在 retained evidence 证明完整 measured path 确实不存在
  该类 fixed/applicable unit 时才为零。Opaque、unobserved、variable 或 undisclosed
  unit 不能用零表示，并使外层 performance record ineligible；其他情况使用至少为一
  的精确 effective value。
- `layout_mode=single` 要求 data/parity/replica/stripe 为 `1/0/1/0`。`striped`
  要求 data 至少为二、parity 为零、replica 为一且 stripe unit 为正；`mirrored` 或
  `replicated` 要求 data 为一、parity 为零、replica 至少为二且 stripe 为零；
  `erasure-coded` 要求 data、parity 与 stripe 都为正，replica 为一。
- `layout_mode=provider-managed` 仍要求一个稳定、非 `none` 的
  `layout_profile`，该 profile 必须在已记录 backend-semantics generation 下标识精确
  effective provider layout 或 service profile；通用 `provider-managed`、opaque、
  unknown 或 undisclosed placeholder 都不是 profile。一个 `known` performance
  payload 中四个 geometry frame 必须全部存在。每个 frame 的正数表示该概念存在于
  完整 measured provider path，而且该数值就是观测到的精确 effective value。零值
  只允许与下列匹配的 retained raw-proof kind 一起出现：

  | Geometry component | 零值要求的精确 raw-proof kind |
  | --- | --- |
  | `layout_data_units` | `provider-layout-data-units-absent` |
  | `layout_parity_units` | `provider-layout-parity-units-absent` |
  | `layout_replica_count` | `provider-layout-replica-count-absent` |
  | `layout_stripe_unit_bytes` | `provider-layout-stripe-unit-absent` |

  每项 proof 必须证明命名概念不存在于完整 effective provider path，而不只是被 API
  boundary 隐藏。这四个 proof kind 是封闭 raw-evidence label，不是新的 manifest
  state/reason pair、N/A 情况、field 或 digest 输入。如果概念存在，但其唯一精确
  effective value opaque、variable、undisclosed 或 unobserved，则整个
  `b1_performance_configuration` field 必须为
  `unprovable/evidence-chain-incomplete` 且 payload 为空；不能用零或部分 37-component
  record 表示。Value 冲突，或 absence proof 与 observed path 冲突时，整个 field 为
  `unprovable/conflicting-effective-values`。Profile、所有正值、所有零值 proof 与完整
  provider path 必须来自同一次冻结 observation，并满足
  `backend_semantics_id`/`backend_semantics_generation` 定义的 profile-specific
  relationship。因此，只有四个精确 absence proof、非 placeholder layout profile
  都存在且相互一致时，全零 geometry vector 才有效；provider opacity 永远不代表
  absence。
- `upper_write_cache_mode=absent` 要求 profile 为 `not-applicable` 并具有 raw
  layer-absence proof；`disabled` 要求 `none`；其他 enabled 或 managed mode 要求其
  精确 profile。Queue/concurrency 为 `serial` 时 value 为一，`fixed` 与
  `provider-managed` 时具有 positive effective bound，`unbounded` 时为零。
  Undisclosed provider limit 不等于 unbounded。
- `network_path=not-applicable` 要求 protocol、link、QoS 与 region 全部为
  `not-applicable`，MTU 为零，并证明完整 storage path 不含 network hop。其他 path
  要求非 sentinel protocol/link/QoS/region identifier 与 positive effective MTU。
  Remote-provider boundary 不能免除这些 field。
- `backend_service` 始终命名精确 effective service。Performance tier 或 device
  profile 只有在 retained proof 表明对应 configurable layer 缺失时才可为
  `not-applicable`。Normalized identifier value 绑定
  `backend_semantics_id` 与 `backend_semantics_generation`；实现不能另造 alias 让两种
  native configuration 被迫比较为相等。

完整 performance configuration 在 warmup 前观测并冻结，且必须在整个 replicate
持续生效。它包含稳定 effective configuration，而不包含瞬时 queue occupancy/latency、
cache temperature、free-space watermark、provider autoscaler/load state、competing-
process load、network RTT/jitter、disposable path/job-directory name 或 subject
repository commit/build/binary identity。这些 time-varying fact 保持为 eligibility/
precondition evidence 或 raw diagnostic；v1 不要求两个运行具有完全相同的背景噪声。
Replicate 内 configuration drift 为 invalid；diagnostic noise 不进入 compatibility
byte。

每个可能改变 B1 payload/manifest write、synchronization、barrier、provider-commit、
revalidation 或 golden-readback timing 的 effective mount、filesystem、volume、
device、backend、provider 或 network option/configuration，都必须映射到固定 mount
map、commit map、performance record、hardware-cache policy 或 PLP policy。纯
application CPU hashing 本身不新增 storage field，但改变用于 revalidation 的 read/
write 的 option 仍在范围内。只有 retained authoritative evidence 证明某 option 对完整
measured path 上的 performance 与 durability 都没有影响时，才可以排除；否则
`b1_performance_configuration` 为 `unprovable/evidence-chain-incomplete`，storage
eligibility 包含 `performance-configuration-unprovable`。例如，Btrfs
`compress=zstd` 与 disabled compression 必须产生不同 performance record，即使七
key mount map、commit map 与 artifact manifest 在其他方面相同，也不兼容。

Base manifest 以 `execution-profile-base-environment-v1\n` 开始，随后精确包含以下
24 条 record。除唯一明确的 N/A 情况外，每条 record 都必须是 `known`：

| # | Field | 精确 type 与 known value domain |
| ---: | --- | --- |
| 1 | `os_family` | `enum`：`darwin`、`linux` 或 `windows` |
| 2 | `os_release` | `text` |
| 3 | `kernel_name` | `text` |
| 4 | `kernel_release` | `text` |
| 5 | `architecture` | `enum`：`aarch64` 或 `x86_64` |
| 6 | `cpu_inventory` | `cpu-record-list-v1`；一条或多条 unique record |
| 7 | `gpu_inventory` | `device-record-list-v1`；零条或多条 unique record |
| 8 | `other_device_inventory` | `device-record-list-v1`；零条或多条 unique record |
| 9 | `compiler_id` | `enum`：`apple-clang`、`clang`、`gcc` 或 `msvc` |
| 10 | `compiler_version` | `text` |
| 11 | `compiler_target` | `text` |
| 12 | `standard_library_id` | `enum`：`libcxx`、`libstdcxx` 或 `msvc` |
| 13 | `standard_library_version` | `text` |
| 14 | `build_mode` | `enum`：`debug`、`release`、`relwithdebinfo` 或 `minsizerel` |
| 15 | `build_flags` | `ordered-text-list-v1`；按 compiler invocation order 编码零个或多个 flag |
| 16 | `process_worker_count` | `uint64`；`1..18446744073709551615` |
| 17 | `provider_contracts` | `contract-record-list-v1`；一条或多条 execution/data/operation/policy provider contract，不含 `OutputStore` |
| 18 | `plugin_contracts` | `contract-record-list-v1`；零条或多条 loaded plugin contract |
| 19 | `resource_limits` | `resource-limits-v1`；下述精确 record |
| 20 | `metal_resource_limits` | `metal-resource-limits-v1`；下述精确 record，或仅以 `configured-metal-executor-absent` 表示 N/A |
| 21 | `cache_preconditions` | `cache-preconditions-v1`；下述精确 record |
| 22 | `residency_preconditions` | `residency-preconditions-v1`；下述精确 record |
| 23 | `power_policy` | `power-policy-v1`；下述精确 record |
| 24 | `thermal_eligibility` | `thermal-eligibility-v1`；下述精确 record |

Inventory 与 contract record 的 component 顺序固定：

- `cpu-record-v1` 是 `(stable_identity:text, vendor:text, model:text,
  firmware:text, physical_cores:uint64, logical_cpus:uint64)`。
- `device-record-v1` 是 `(stable_identity:text, class:enum, vendor:text,
  model:text, driver_contract_id:identifier,
  driver_contract_generation:uint64)`；`class` 为 `gpu`、`accelerator` 或 `io`。
- `contract-record-v1` 是 `(contract_id:identifier,
  contract_generation:uint64, semantics_id:identifier,
  semantics_generation:uint64)`，两个 generation 都至少为一。

`resource-limits-v1` 按以下顺序精确包含这些 component 与 value：
`cpu_slots=32`、`host_retained_limit_bytes=1073741824`、
`host_scratch_limit_bytes=536870912`、`ready_entry_limit=65536`、
`ready_byte_limit=268435456`、`interactive_headroom_cpu_slots=1`、
`interactive_headroom_host_retained_bytes=67108864`、
`interactive_headroom_host_scratch_bytes=33554432`、
`interactive_headroom_ready_entries=1024`、
`interactive_headroom_ready_bytes=16777216`、`compute_io_task_limit=64` 与
`compute_io_planned_byte_limit=268435456`；每个 component type 都是 `uint64`。
`metal-resource-limits-v1` 精确为 `(executor:enum=metal,
device_memory_limit_bytes:uint64=536870912,
device_scratch_limit_bytes:uint64=268435456)`。

其余 fixed record 是：

- `cache-preconditions-v1` 为 `(disk_cache:enum=disabled,
  codec_io:enum=disabled, cross_episode_result_reuse:enum=disabled,
  cross_job_result_reuse:enum=disabled)`。
- `residency-preconditions-v1` 为 `(i1_host:enum=baseline-and-current,
  i2_host:enum=baseline-preview-final,
  i2_metal:enum=conditional-first-upload-then-reuse,
  b1_result_reuse:enum=disabled,
  m1_execution_authority:enum=single-process-domain)`。
- `power-policy-v1` 为 `(source:enum, mode:enum, sleep:enum)`，其中 `source` 为
  `external-ac` 或 `battery`，`mode` 为 `automatic`、`balanced`、
  `high-performance` 或 `low-power`，`sleep` 为 `inhibited` 或 `allowed`。
- `thermal-eligibility-v1` 为 `(start:enum, maximum_allowed:enum)`；每个 component
  都是 `nominal`、`fair`、`serious` 或 `critical`。

Repository commit、dirty state、executable/library/provider/plugin binary hash、
bundle 与 row identity 以及 disposable path 仍是必需 raw subject/audit evidence，
但不是 base 或 storage manifest field。这样 candidate 与 reference subject binary
可以不同，而 compiler/build configuration 与 contract generation 必须相同。
Same-subject M1 pair 仍单独比较这些 raw subject identity。

小写 digest 定义是精确的：

```text
storage_environment_digest = lowerhex(SHA-256(exact storage manifest bytes))
base_environment_digest = lowerhex(SHA-256(exact base manifest bytes))
environment_class_digest = lowerhex(SHA-256(exact environment-class manifest bytes))
```

Environment-class manifest header 精确为
`execution-profile-environment-class-v1\n`。它按以下顺序精确包含四条 record：
`base_environment_digest`（`sha256`）、
`storage_environment_applicability`（`enum`）、
`storage_environment_not_applicable_reason`（`enum`）与
`storage_environment_digest`（`sha256`）。B1/M1 编码 known value `required`、
`none` 与复算的 storage digest。Applicability 的精确 domain 是 `required` 或
`not-applicable`；N/A-reason 的精确 domain 是 `none` 或
`row-has-no-output-commit`。I1/I2 编码 known value `not-applicable` 与
`row-has-no-output-commit`；最后一条 digest record 的 state 为 `not-applicable`，
reason 相同，payload 为空。任何行都不遗漏四条 record 中的任意一条。

Storage compatibility eligibility 是 derived evidence，不是 digest 输入。Reason
list 是确定性结果，不是 producer 自选 subset：

1. Validator 首先完成整个 canonical storage manifest 的 parsing 与 validation，
   覆盖 framing、lexical form、field/type/state/reason rule、scalar/composite
   domain、cardinality、ordering/uniqueness、fixed-record shape 与全部 cross-field
   rule。该阶段任一失败都会使 storage 为 `ineligible`，reason list 精确且仅为
   `canonical-schema-invalid`，并立即停止 eligibility evaluation，因为此时无法安全
   评估 raw-evidence 或 semantic predicate。
2. 对 canonical manifest，validator 独立评估下表每个 predicate，并且只输出所有为真
   的 token，每项恰好一次，按 unsigned-ASCII 排序。完整可能顺序精确为
   `canonical-schema-invalid`、`commit-semantics-inconsistent`、
   `durability-class-not-crash-durable`、`durability-path-inconsistent`、
   `mount-normalization-unprovable`、`not-applicable-proof-invalid`、
   `performance-configuration-unprovable`、
   `raw-observation-proof-incomplete`、`required-capability-absent`、
   `required-observation-ineligible`、`root-containment-unproved`。空 list 表示
   `eligible`；任何非空 list 都表示 `ineligible`。

Canonical-manifest 阶段的 predicate 精确定义如下：

| Reason token | 为真的精确 predicate |
| --- | --- |
| `commit-semantics-inconsistent` | 六个 known commit-semantic value 与 retained transaction/receipt observation 无法在已记录 backend semantics 下共同描述一个内部一致的 payload-stage、manifest-last、no-replace、synchronization 与 leaf-to-root/provider-transaction commit。Capability 缺失不属于此 predicate。 |
| `durability-class-not-crash-durable` | `requested_durability` 或 `achieved_durability` 至少一个为 `known` 且不是 `crash-durable`。没有 known 较弱 value 的 ineligible observation state 由 `required-observation-ineligible` 处理。 |
| `durability-path-inconsistent` | Known contract/backend/instance/mount、endpoint、anchor、commit 与 retained receipt/path fact 明确标识互相冲突的 path，或无法形成一条端到端 durability path。仅缺少 raw binding proof 由 `raw-observation-proof-incomplete` 处理。 |
| `mount-normalization-unprovable` | Present mount 因 `mount_identity` 或 `mount_effective_options` 为 `unprovable`、default/case/duplicate/unknown-option resolution 未解决，或 retained native observation 与 canonical mount mapping 冲突，无法唯一归约到 known identity 与七 key effective map。已证明 absent 的 mount 使用 N/A predicate。 |
| `not-applicable-proof-invalid` | 至少一个语法允许的 N/A field/reason pair 缺少精确完整路径 layer-absence proof，或该 proof 与 retained path observation 冲突。 |
| `performance-configuration-unprovable` | Performance field 不是 `known`；某个 effective performance/durability option 缺少完整 mapping 或 no-effect proof；provider-managed geometry value/zero proof 不完整；或冻结 configuration 在 replicate 中 drift。即便 raw observation stream 完整，已观测 drift 也映射到这里。 |
| `raw-observation-proof-incomplete` | 用于证明 `known` storage value、允许的 N/A claim 或 canonical normalization 的 raw observation/raw-to-canonical mapping proof 缺失、不完整、陈旧或与 canonical value 冲突。这不是 schema failure、read-only access、capability 缺失、known 较弱 durability class、证据完整的 commit/path inconsistency、已观测 configuration drift 或 root containment 的兜底项。 |
| `required-capability-absent` | Known effective `access_mode` 为 `read-only`，或八个封闭 durability capability token 中至少一个缺失。 |
| `required-observation-ineligible` | 至少一个 required storage field 的 state 为 `unknown`、`unobserved`、`unsupported` 或 `unprovable`。语法允许的 N/A state 只由其 proof predicate 评估。 |
| `root-containment-unproved` | 任一 measured job 或 retained release-artifact destination 缺少所选 `OutputStore` root 之下成功且无歧义的 proof，或 retained proof 失败/冲突。Containment proof 归此 predicate，而不归 generic raw-mapping predicate。 |

重叠是刻意且确定的。`unprovable` mount 或 performance field 还会触发
`required-observation-ineligible`；mount/performance raw mapping 缺失或冲突会触发其
特定 token 与 `raw-observation-proof-incomplete`；无效 N/A absence proof 会触发
`not-applicable-proof-invalid` 与 `raw-observation-proof-incomplete`。因此 mount 冲突
可能输出全部三个适用 token。Commit 或 durability-path contradiction 输出其特定
token；只有 raw-to-canonical binding 本身缺失或冲突时才另外输出 raw-proof token。
Reason list 仍排除在所有 environment digest 外，但 canonical manifest 与 retained raw
evidence 必须让独立 validator 精确复现它。

只有当两侧都 eligible、保留的 canonical storage manifest 逐 byte 相同、各自提供的
digest 等于独立复算值，而且两个 digest 相等时，两个 storage environment 才兼容。
该 byte comparison 必然包含完整 framed `b1_performance_configuration` field。
Digest 相等绝不替代 byte 相等。Base compatibility 对 base manifest 使用同一精确
byte 与复算 digest 规则。Candidate/reference I1 或 I2 comparison 要求精确 base
compatibility 与固定 storage-N/A environment-class manifest。Candidate/reference
B1/M1、B1 cap-1/cap-8，以及 M1/paired-B1-cap-8 comparison 要求精确 base 与
storage compatibility，并要求完整 environment-class manifest 与复算 digest 相等。
M1/paired-I1 只比较精确 base compatibility；二者的 environment-class manifest
有意不同，M1 storage 不能使该 I1 latency pair 无效。

Issue #95 负责完整 storage path（包括 performance configuration）的固定 raw probe-
to-schema mapping、映射到该精确 schema 的 backend adapter、mount normalization、
state/reason proof、唯一 canonical encoder 与 digest 生产、eligibility/root-
containment evidence，以及 B1 cap-1/cap-8 和 candidate/reference 检查。Issue #96
原样复用这些 byte，在 warmup 前记录 M1 storage observation，并在保留 base-only
M1/I1 pairing 的同时强制执行精确 same-ordinal M1/B1 pair。两个 Issue 都不能新增
v1 field、重新解释 sentinel 或定义替代 provider grammar。Issue #92 不新增 probe、
serializer、API、runtime result field、harness 或 compatibility code。

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
- 对 B1 与 M1，所选 `OutputStore` root evidence、包含冻结 B1 performance
  configuration 的规范化 storage fingerprint、`storage_environment_digest`、
  compatibility eligibility 与 raw capability/configuration observation。

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

#### M1 cold 与 warmup input grid 是精确合同

M1 从已经保留的测量边界派生两个额外且经过 checked arithmetic 的 monotonic
boundary：

```text
C^M1 = B^M1 - 6,000,000,000 ns
W^M1 = B^M1 - 5,000,000,000 ns = C^M1 + 1,000,000,000 ns
```

对应 row-local event sequence 为 `c^M1` 与 `w^M1`，并满足
`(C^M1,c^M1) < (W^M1,w^M1) < (B^M1,b^M1)`。Checked subtraction/
addition 失败会使结果无效。Cold、warmup 与 measured interval 精确为
`[(C^M1,c^M1),(W^M1,w^M1))`、
`[(W^M1,w^M1),(B^M1,b^M1))` 与
`[(B^M1,b^M1),(U^M1,u^M1))`；runner 不能另选 origin 或时长。

在 `(C^M1,c^M1)`，一个零时长 start transaction 建立唯一 cold I1 nominal
origin `E^M1_cold=C^M1`，并精确 offer B1 Graph A seed 252；其
`phase=cold`、`cycle_ordinal=0`、`attempt=0`。该 B1 offer 排在 `c^M1`
之后；同 timestamp 的 cold I1 admission 排在该 offer 之后。Cold I1 occurrence
使用既有 I1 offset，并在 `C^M1+683,333,337 ns` 关闭自身 settlement window，
因此在 `W^M1` 前保留固定 316,666,663 ns guard。其 generation 必须在该 endpoint
达到 quiescence。Seed 252 的唯一 terminal endpoint、artifact owner settlement 与
output removal 必须全部排在 `(W^M1,w^M1)` 前。Miss 不会移动 `W^M1` 或插入
drain，而是令 replicate invalid。

在 `(W^M1,w^M1)`，第二个零时长 transaction 验证 cold work 已经满足上述
endpoint，关闭 cold source，建立 warmup I1 origin `E^M1_warmup,0=W^M1`，并
启用有限的 warmup B1 protocol。随后先向 Graph B offer seed 253，再向 Graph A
offer seed 254；二者均使用 `phase=warmup`、`cycle_ordinal=0`、`attempt=0`，且
满足 `w^M1 < sequence(B253) < sequence(A254)`。同 timestamp 的第一次 warmup
I1 admission 排在两次 offer 之后。当且仅当 B253 terminal 时，Graph B producer
才在同一 timestamp、以更大的 sequence 同步 offer B255，并沿用同一 warmup cycle/
attempt。B255 必须在 `(B^M1,b^M1)` 前已经 offered；否则固定 warmup fixture 不
完整，replicate invalid。Graph A 在 A254 后没有 warmup successor。因此，完整
offered warmup prefix 由 protocol 固定，而 `B^M1` 处 incomplete subset 只由保留的
terminal history 决定，绝不能由 runner 选择。

Warmup I1 精确具有七个 nominal origin：
`E^M1_warmup,k=W^M1+k*750,000,000 ns`，其中 `k=0..6`。Origin `k=0..5`
的 per-occurrence settlement window 均在下一 origin 前关闭。Origin `k=6` 精确为
`B^M1-500,000,000 ns`；其固定 `Q_end=B^M1+183,333,337 ns`，所以该
occurrence/generation 在测量边界仍处于 settlement-pending 状态。它保持不可变的
`phase=warmup`，并进入 `B^M1` carryover snapshot。在其 `Q_end`，endpoint snapshot
只适用于该 warmup occurrence/generation：它必须达到 quiescence 并完成 settlement，
但并发活跃的 measured generation 与 shared execution service 无需全局为空。Cold 与
warmup transition 都不得 restart process、cool provider、rebuild queue、release shared
resource 或移动 boundary。

#### M1 测量边界保留 warmup carryover

M1 保留一个 exact monotonic timestamp 为 `B^M1=M_0`、row-local sequence 为
`b^M1` 的 boundary event，以及一个 checked timestamp 为
`U^M1=B^M1+30,000,000,000 ns`、sequence 为 `u^M1` 的 terminal-cutoff event。
每个 raw boundary/lifecycle event 都有唯一且严格递增的 row-local sequence。Event
coordinate 按 `(monotonic_timestamp,event_sequence)` 排序，因此具有相同 clock value
的并发 lifecycle event 仍无二义性。三十个一秒 window 占用有序 interval
`[(B^M1,b^M1),(U^M1,u^M1))`。Checked addition 失败会使结果无效。

在 `B^M1`，一个零时长 boundary transaction 按以下逻辑顺序执行，并且不会停止
shared execution domain：

1. 关闭每个 warmup offer source，即 warmup I1 cadence 与两个 B1 Graph producer，
   因此在该 boundary event 或其后不再 offer warmup occurrence；
2. 对 boundary 前已经 offered、但唯一 completion endpoint 尚未排在 boundary 前的
   每个 warmup occurrence 取得 snapshot；snapshot 包含完整 `job_instance_id` 或 I1
   episode/generation identity、offered-waiting/accepted/queued/running state、queue
   predecessor、reservation/grant 与 owner-settlement state；
3. 只重置 measured logical accumulator，同时保留 raw event、occurrence identity、
   queue、policy state、resource authority 与 carryover snapshot；
4. 把第一个 measured I1 nominal origin 建立在 `M_0`；
5. offer measured B1 Graph A job zero，随后 offer Graph B job one；二者 timestamp
   都是 `B^M1`，且 sequence value 满足
   `b^M1 < sequence(Graph A job zero) < sequence(Graph B job one)`；二者均使用
   `phase=measured`、producer-local `cycle_ordinal=0` 与 `attempt=0`。

步骤一至四在 boundary coordinate 形成一个 atomic logical transition；任何其他
row-local lifecycle event 都不能插入其 snapshot 或 counter reset。Timestamp 为
`B^M1` 且 sequence 小于 `b^M1` 的 lifecycle event 排在整个 transition 前；sequence
大于 `b^M1` 的 event 排在其 snapshot/reset 后，再按 sequence 与两个 measured B1
offer 排序。

第一次 measured-I1 actual admission 不属于 atomic snapshot。若其 timestamp 等于
`B^M1`，其 sequence 必须排在两次 measured B1 offer 之后。因此，其被接受的
latest-wins supersession 也排在冻结的 carryover snapshot 之后，不能移除或改写其中
记录的 warmup generation。由该旧 generation 因果产生的 cancellation、terminal 或
settlement event 都保留更晚的 event sequence 与 warmup phase。

同 timestamp 的 lifecycle event 排在 boundary 前时，snapshot 反映其新 state；排在
其后时，它是 cross-boundary event。同 timestamp 的 terminal warmup event 绝不会在
步骤一之后创建新的 warmup successor。Phase boundary 不包含 wait、cooling interval、
drain、cancellation、process restart、worker/policy/queue reconstruction 或 resource
release。普通 measured-I1 admission 可以按冻结的 latest-wins 规则 supersede 保留的
warmup I1 generation，但只能发生在 cutoff/snapshot 之后，并遵循上述顺序；harness
不会增加只在 boundary 执行的 cancellation。

每个 outstanding warmup B1 occurrence 保留不可变的 `phase=warmup`、cycle、job、
attempt identity 及现有 per-Graph FIFO 位置。新的 measured offer 即使面对仍 queued
或 running 的完整 warmup prefix，也排在该 Graph prefix 之后。这个精确 transition
是唯一无需等待 predecessor terminal 即可 offer 的例外。随后，Graph A 在 measured
predecessor terminal 时 offer 下一个偶数 job，并从 job 28 直接进入自己下一轮
producer-local cycle 的 job zero；Graph B 则独立地从 job 29 进入 job one。任一
producer 都不等待另一个，也不完成、递增或改写未完成的 warmup identity。Cap-eight
admission bound、active backlog、queue order 与 resource ownership 跨 boundary 保持
不变。

Occurrence-owned result 按不可变 phase 归因，绝不按 completion timestamp 归因。
即使在 `B^M1` 后观察到，warmup occurrence 的 terminal result、completed service、
output byte、latency、receipt、golden/digest result、duplicate/retry/discarded service
与 owner settlement 仍是 warmup evidence；它们不进入 measured throughput、
completed-service fairness `x`、latency、determinism 或 waste 的 numerator/denominator。
Measured occurrence endpoint 只有在其有序 event coordinate 位于 measured interval
内时才产生贡献。与之不同，scheduler 与 resource observation 按时间 window 归属：
`B^M1` 之后的 actual class-start ordering、headroom failure、queue contention、active
reservation/grant、Compute I/O count 与 Host/device/ready-memory high-water 包含每个
phase 的物理影响。因此 carryover 不能从 contention 或 memory evidence 中隐藏。
Class-start bound 会观察 measured interval 内每一次 actual Throughput start，包括
warmup start；Jain completed service 则只使用 measured-occurrence service。

Warmup evidence 仍是必需项：carryover failure、event evidence 缺失、event sequence
重复、event coordinate 无法形成全序、非法 phase rewrite、boundary-only
cancellation、queue reorder、snapshot mismatch 或无法证明 settlement，都会使
replicate 无效，即使它的 occurrence-owned 数量不进入 measured aggregate。在
`(U^M1,u^M1)`，有序 cutoff 会停止新的 measured B1 offer，但不取消已经 offered 的
work。排在该 cutoff 或其后的 endpoint 会被保留，但不进入 30-second numerator。
Teardown 必须 drain 全部 phase，并达到既有 resource/Compute-I/O exact-zero
settlement；`B^M1` 刻意不要求 quiescence。

Workload manifest 保留 `C^M1`、`W^M1`、`B^M1`、`U^M1`、精确 phase interval、
I1 origin/count/index 与 `Q_end` 算术、cold/warmup B1 offer protocol、event-order/tie
rule、boundary step order、queue/carryover policy、producer-local cycle rule 与 phase-
attribution rule。Measurement evidence 保留四个 boundary event、每次固定 cold/
warmup offer 及由 actual terminal 派生的 prefix transition、完整 carryover snapshot、
每个 tied event coordinate/state transition、首批 measured offer、per-Graph predecessor
与 next-cycle counter、queue/start/terminal/receipt join、counter epoch、resource sample、
failure 与最终 settlement。既有 section 与 verdict digest 覆盖这些 byte；封闭的
15-field row 与五 field bundle 不变。在继续使用 `M1-shared-v1` 时，任何 origin、
offer、cycle、boundary、ordering、carryover、attribution 或 evidence drift 都会使结果
无效；有意改变时必须创建新 workload id。

Cold first use 单独保留，绝不混入 steady-state aggregate。全部 duration 使用
monotonic clock。Percentile 使用 nearest rank：排序 `N` 个 sample，并选择从一开始的
rank `ceil(p*N)`。每个 replicate 必须独立通过；不得通过 pooling 隐藏坏进程。摘要
可以报告三个 replicate aggregate 的中位数。

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
每个 ordinal 的 candidate 与 reference B1 行都必须使用上述封闭 schema 下的兼容
storage fingerprint。同一 subject 内 B1 cap-1/cap-8 determinism 比较也要求相同且
兼容的 fingerprint；Run cap 是有意存在的差异，storage 不是。

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

Environment class 按行确定适用范围，而不是一个没有限定的 machine label。上文
已经固定精确的 24-field base manifest、21-field storage manifest、四 field
environment-class manifest、record grammar 与 digest 输入。Base manifest 绑定
OS/kernel、architecture、inventory、compiler/build configuration、worker count、
execution provider/plugin contract、冻结 resource、cache/residency precondition 与
power/thermal eligibility。它排除 repository commit 与 binary artifact hash，因为
这些 identity 标识的是被比较 subject，而不是其受控 environment。

每个 bundle 保留全部适用的 canonical manifest、claimed 与独立复算 digest、raw
observation、normalization proof、eligibility result/reason 以及 root-containment
evidence。I1/I2 使用固定 storage-N/A environment-class encoding；B1/M1 使用必需
storage digest。缺少 record、manifest、digest、raw observation、proof、eligibility
reason、精确 byte match 或独立 digest match 中的任一项，都会使受影响 relative
verdict 成为 `invalid`。只有稳定规范化 field 匹配且双方 containment proof 成功时，
不同 disposable absolute path 才可比较；相同 path string 绝不能覆盖 manifest drift。

#### Evidence Row 与 Bundle Byte 是封闭的 v1 Schema

只有通过 canonical byte 才能复现 content address。因此，evidence envelope 复用上文
精确的 `field=<frame(...)>...\n` grammar、scalar lexical rule、fixed-record grammar、
list framing、state/reason rule 以及 no-BOM/final-LF rule。Row 与 bundle manifest
不允许 JSON、provider-native object、omitted field、`null`、reordered field 或
extension field。只对本小节的 digest formula，`frame(O)` 可以接收任意 octet
sequence `O`，并使用其 byte count；field-record name、metadata 与 canonical
manifest 仍按上文要求保持 ASCII。

Evidence row 以精确 ASCII header `execution-profile-evidence-row-v1\n` 开始，随后
包含且只包含以下 15 个 record：

| # | Field | 精确 type 与 known value domain | 允许的 N/A |
| ---: | --- | --- | --- |
| 1 | `workload_id` | `workload-id-v1`；四个冻结 token 中精确的一个 | 否 |
| 2 | `subject_role` | `enum`：`candidate` 或 `reference` | 否 |
| 3 | `replicate_ordinal` | `uint64`；`1..3` | 否 |
| 4 | `run_cap` | `uint64`；workload row 冻结的 cap | 否 |
| 5 | `base_environment_digest` | `sha256`；独立复算 | 否 |
| 6 | `storage_environment_applicability` | `enum`：`required` 或 `not-applicable` | 否 |
| 7 | `storage_environment_digest` | `sha256`；适用时独立复算 | `row-has-no-output-commit` |
| 8 | `environment_class_digest` | `sha256`；独立复算 | 否 |
| 9 | `workload_manifest_digest` | `sha256`；下文定义的 section digest | 否 |
| 10 | `job_instance_index_digest` | `sha256`；下文定义的 section digest | 否 |
| 11 | `measurement_evidence_digest` | `sha256`；下文定义的 section digest | 否 |
| 12 | `output_evidence_digest` | `sha256`；下文定义的 section digest | 否 |
| 13 | `verdict_evidence_digest` | `sha256`；下文定义的 section digest | 否 |
| 14 | `paired_isolated_i1` | `evidence-pair-reference-v1` | `row-has-no-isolated-pair` |
| 15 | `paired_isolated_b1_cap8` | `evidence-pair-reference-v1` | `row-has-no-isolated-pair` |

Environment record 重复精确的适用 manifest digest，使 row 无需隐式 machine label
即可验证。I1/I2 使用上文已经定义的精确 storage-N/A state/reason/zero-byte payload。
两个 pair record 只在 M1 中为 known；每个非 M1 row 都把二者编码为
`not-applicable/row-has-no-isolated-pair`，并使用 zero-byte payload。任何 digest 都
不能用空 string 作为 sentinel。

`evidence-pair-reference-v1` 是一个 fixed record，其 payload 按顺序拼接
`(row_digest:sha256,bundle_digest:sha256,replicate_ordinal:uint64)` 的 frame。
`job-instance-list-v1` 使用 generic list grammar，并在每个完整 `job-instance-v1`
payload 外加一个 frame。Item 按 phase rank `cold < warmup < measured`、数值
`cycle_ordinal`、数值 `job_index`，最后按剩余完整 payload byte 排序；重复完整
payload 或重复 `(phase,cycle_ordinal,job_index)` coordinate 都无效。Retained section
byte 精确由 ASCII header `execution-profile-job-instance-index-v1\n` 与唯一一个 field
record 构成；该 record 名为 `job_instances`，state 为 `known`，reason 为 `none`，
type 为 `job-instance-list-v1`，payload 为 canonical list。I1/I2 把 known empty list
编码为 payload `0:`；B1/M1 编码每个 offered B1 occurrence，包括 cold 与 warmup。
该 index 是从 occurrence identity 到 charge、admission、output、receipt、trace、
aggregate 与 verdict evidence 的权威 join。

五个 section field 使用以下精确 `(section_name,section_schema_id)` pair：

| Row field | `section_name` | `section_schema_id` |
| --- | --- | --- |
| `workload_manifest_digest` | `workload-manifest` | `execution-profile-workload-manifest-v1` |
| `job_instance_index_digest` | `job-instance-index` | `execution-profile-job-instance-index-v1` |
| `measurement_evidence_digest` | `measurement-evidence` | `execution-profile-measurement-evidence-v1` |
| `output_evidence_digest` | `output-evidence` | `execution-profile-output-evidence-v1` |
| `verdict_evidence_digest` | `verdict-evidence` | `execution-profile-verdict-evidence-v1` |

对每个 field，精确 retained section octet（包括其 versioned section schema 允许时显式
known-empty collection）按下式生成 digest：

```text
section_digest = lowerhex(SHA-256(
  "execution-profile-evidence-section-digest-v1\n" ||
  frame(section_name) || frame(section_schema_id) || frame(section_bytes)))
```

Row 存储该值。Retained section byte 是强制证据，且必须能够复算该值；digest 绝不
替代 section。#93 至 #96 拥有各自 collector 对应的 versioned inner record，但不能
改变本 envelope、domain separator、section-name binding 或 occurrence join。

每个 versioned retained-section schema 与 bundle-provenance schema 都必须封闭其
address-bearing dependency。只要 canonical byte 复制、命名或以其他方式派生自另一
object 的 content address，就存在该 dependency；不要求最终十六进制 digest 以字面形式
出现。Schema 必须标识每个 typed address-bearing field，以及生成 canonical byte 时使用的
每个 address input。存在 opaque 或未分类的 address-bearing field、遗漏 dependency，或
producer 无法证明完整 dependency set 时，该 section 与所有依赖 verdict 都成为 `invalid`。

Evidence bundle 以精确 ASCII header
`execution-profile-evidence-bundle-v1\n` 开始，随后包含且只包含以下五个 record：

| # | Field | 精确 type 与 known value domain | 允许的 N/A |
| ---: | --- | --- | --- |
| 1 | `workload_id` | `workload-id-v1`；四个冻结 token 中精确的一个 | 否 |
| 2 | `subject_role` | `enum`：`candidate` 或 `reference` | 否 |
| 3 | `bundle_provenance_digest` | `sha256`；对 retained repository/build/binary/provider provenance 计算的 section digest | 否 |
| 4 | `comparison_reference_bundle_digest` | `sha256`；candidate 使用的 immutable external reference | `reference-has-no-comparison-baseline` |
| 5 | `row_references` | `row-reference-list-v1`；非空 canonical list | 否 |

`bundle_provenance_digest` 使用上文 section formula，且
`section_name=bundle-provenance`、
`section_schema_id=execution-profile-bundle-provenance-v1`。
`row-reference-v1` 是 fixed record，component 按精确顺序排列为
`(workload_id:workload-id-v1,run_cap:uint64,replicate_ordinal:uint64,row_digest:sha256)`。
`row-reference-list-v1` 使用 generic list grammar，在每个完整 row-reference payload
外加一个 frame。其功能行 key 精确为
`(workload_id,run_cap,replicate_ordinal)`。该 list 非空，并按此 key 保持功能唯一；
即使 `row_digest` 不同，具有相同 key 的两个 item 也无效。完整 payload 重复同样无效。
List 按数值 run cap、数值 replicate ordinal，最后按完整 payload byte 排序；每个 item
的 workload id 必须等于 enclosing bundle 的 `workload_id`。

在比较 workload 是否相等、构造 functional key 或查找 target 之前，每个 row、
bundle、job-instance 与 row-reference workload component 都必须按
`workload-id-v1` 解析。
Generic `identifier` type frame、大小写变体或未知 workload 都会使 canonical validation
失败；verifier 不得仅因两个 invalid byte string 相等而绕过该失败。

对每个 item，verifier 必须把 `row_digest` 解析到恰好一个 retained canonical row，
复算 row digest，解析全部 15 个 field，并要求 parsed row 的 `workload_id`、`run_cap`
和 `replicate_ordinal` 等于 item，且其 `subject_role` 等于 enclosing bundle。解析出零个
或多个 row、digest mismatch，或任一 item/row/bundle mismatch 都会使 bundle 无效。
Candidate 编码 known external `comparison_reference_bundle_digest`；reference 则编码
`not-applicable/reference-has-no-comparison-baseline` 与 zero-byte payload。对 candidate，
在查找任何 target row 之前，verifier 必须把其 comparison digest 解析到
恰好一个 retained bundle object。它必须把该 object 解析为上文精确 canonical header
与五个 record，独立复算其 `bundle_digest`，并要求结果等于 candidate 的 claim。解析出的
object 必须具有 `subject_role=reference`，且 `workload_id` 与 candidate 相同；其完整非空
row-reference list 必须通过 canonical ordering、功能 key 唯一性，以及每个 exact-one
row、15-field parse、rehash 与 item/row/bundle 检查。解析出零个或多个 retained object
（包括多个 object 携带相同 digest claim）、五 record parse/schema failure、claimed/
recomputed digest mismatch、role 或 workload 错误，或 row list 无效，都会使全部相关
reference-relative verdict 成为 `invalid`；verifier 不得按 path、insertion order 或 byte
相等从中选择一个 object。

只有 bundle resolution 成功后，每个用于 reference-relative verdict 的 candidate row
才选择功能行 key 相同的恰好一个 reference row。Comparison bundle digest 只标识 target
bundle，绝不负责选择 row。Key 缺失或重复，或解析出的 target row 未通过相同 item/row/
bundle 检查时，相关 reference-relative verdict 成为 `invalid`。

令 `row_manifest_bytes` 和 `bundle_manifest_bytes` 表示从 header 到 final LF 的完整
canonical byte。其 content address 精确为：

```text
row_digest = lowerhex(SHA-256(
  "execution-profile-evidence-row-digest-v1\n" ||
  frame(row_manifest_bytes)))
bundle_digest = lowerhex(SHA-256(
  "execution-profile-evidence-bundle-digest-v1\n" ||
  frame(bundle_manifest_bytes)))
```

这些 formula 中的 quotation mark 只是记法：二者之间的 byte（包括显示的 LF）进入
输入，quote character 本身不进入。

Content address 按以下单向顺序封存：

1. 递归解析并验证每个 immutable external prerequisite row 与 bundle；
2. 按 dependency 拓扑顺序冻结每个 local retained section 与 bundle-provenance byte，
   随后计算各自 section digest；
3. 从已封存 section digest 与允许的已封存 external pair address 构造并冻结每个 row
   manifest，随后计算其 row digest；
4. 从已封存 provenance、已封存 row 与允许的已封存 comparison reference 构造并冻结
   bundle manifest，随后计算其 bundle digest；以及
5. 把 claimed row/bundle digest 发布在 immutable canonical object 旁边，绝不放入其中。

任何阶段都不得使用 fixed-point search、改写已经封存的 object，或使用晚于依赖 object
才封存的 address。Address-dependency graph 为每个 retained section、provenance object、
row 与 bundle 建立一个 node。Edge `X -> Y` 表示 `X` 的 canonical byte 或 content
address 依赖 `Y` 的 content address。该 graph 必须有限且无环，并且每条 edge 的 target
都必须在 sealing order 中先于 source。Section 或 provenance node 只能依赖已封存
prerequisite；不得直接或传递可达其 enclosing row、enclosing bundle，或任何尚未封存/
较晚 stage node。Row 可以依赖自身已封存 section 与已封存 external pair target，但不能
依赖 enclosing bundle。Bundle 可以依赖自身已封存 provenance、已封存 row 与已封存
comparison target。任何从 node 直接或传递回到自身的 path 都无效。

Claimed `row_digest` 与 `bundle_digest` 刻意不进入自身 manifest byte。Bundle 包含 row
digest，但绝不包含自己的 digest。External bundle graph 从 enclosing bundle 指向其
`comparison_reference_bundle_digest` 或任一自有 row 中 M1 pair 命名的每个 bundle。
每个 target 都必须已经物化并封存，完整 comparison/M1 graph 必须全局无环；只检查直接
target 不充分。

每个 known M1 pair 都必须解析其 bundle 与精确命名的 row digest，随后通过相同 canonical
item/row/bundle 一致性检查。Target bundle 与 row 的 `subject_role` 必须等于 enclosing
M1 bundle role；pair 与 target row ordinal 必须等于 M1 row ordinal；对应 pair 的 target
功能 key 必须使用 cap 8 的 `I1-edit-storm-v1` 或 cap 8 的 `B1-immutable-v1`。缺少或
存在多个 canonical object、功能 key 缺失或重复、任何 claimed/recomputed mismatch、
未声明 address dependency、later-stage/enclosing dependency，或任一直接/传递 cycle，
都会使全部依赖 verdict 成为 `invalid`。这样，独立 verifier 无需信任 producer-assigned
id，即可复算 section、row、bundle、comparison 与 pairing identity。

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
都必须具有逐 byte 相同的 canonical base manifest 与相等的独立复算
`base_environment_digest`。Paired I1 fixture hash 必须等于 M1
内嵌的 I1 component，但 I1 行保持
`storage_environment_applicability=not-applicable`；M1 的无关 storage field 不参与
I1 latency pair，也不会使其无效。Paired B1 fixture/corpus/golden hash 与 Run cap 8
必须等于 M1 B1 component，而且 M1/B1 pair 必须具有相同的完整
`environment_class_digest` 与精确 storage compatibility。Pair 缺失、为零、ordinal
错误、跨 subject，存在 unknown/unobserved/unsupported/unprovable storage state，
或其他不兼容证据，都会使受影响 M1 relative verdict 成为 `invalid`。

所有被引用 bundle 与 row 都不可变，并按 content digest 选择。未记录的“known
good” build 重跑与 Markdown summary 都不是规范 reference。Raw evidence 必须能够
复算每个 aggregate 与 verdict。

### 后续 Issue 负责固定的证据行

| Issue | 必需 v1 交付 |
| --- | --- |
| #93 | 实现连续 221-slot isolated-I1 grid、精确 `S_11` drain/tie/guard 行为、I1 request/current-generation 与 cancellation/quiescence 观测；发布 isolated latency、waste 与 memory 行，以及必需的 output-correctness 证据。 |
| #94 | 在此处冻结的精确 100-episode/12-edit cadence、acceptance/deadline anchor、preview-before-next-edit ordering，以及 I1 coefficient/index/update/full-resolution-final lineage 上实现 I2；不得重新定义这些 schedule，也不得为 edit `0..10` 选择不同 coefficient 后仍保留 `I2-progressive-v1`。发布 preview/final latency、Host/条件式 Metal residency 与 copy-waste、memory 行，以及必需的 output-correctness 证据。 |
| #95 | 实现 B1 immutable manifest、occurrence-scoped job/task identity、reservation、canonical semantic trace、crash-durable artifact commit、固定 storage/performance probe-to-schema adapter、mount normalization、唯一 encoder/digest、eligibility/B1 check 与 logical/raw golden；在 Run cap 1 与 8 下发布 closed-schema isolated throughput、determinism、zero-fault waste 与 memory 行。 |
| #96 | 把精确 I1 与 B1 fixture 组合为 M1；实现固定的 `C^M1`/`W^M1` cold/warmup origin、count、B1 offer protocol、跨 `B^M1` I1 settlement，以及精确 cutoff/carryover/FIFO/phase-attribution 与 temporal-resource boundary；把既有 `cycle_ordinal` component 解释为每个 measured B1 Graph 的独立 producer-local counter，且绝不把它当作 retry 或新增 field；原样复用精确 v1 manifest byte，强制执行 same-ordinal 完整 M1/B1 environment pair，同时让 I1-only pair 只比较 base，并发布 closed-schema mixed latency、throughput progress、fairness、waste 与 memory 行。 |

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
