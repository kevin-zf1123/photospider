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
| `I1-edit-storm-v1` | 使用 seed zero。从零开始的 edit `i` 从 `[0.82, 1.18, 0.86, 1.14, 0.90, 1.10, 0.94, 1.06, 0.98, 1.02, 0.96, 1.04]` 取值设置第一个 node 的 `k`，并标记 Region `(256*(i mod 4), 256*floor(i/4), 256, 256)`。12 次 edit 在精确的 `(Graph, target node four, GlobalHighPrecision)` supersession key 下以 16,666,667 ns 间隔提交，采用 Interactive QoS、weight 1、Run cap 8，并为每次 edit 使用相对 150 ms monotonic deadline。只有第 12 次 edit 必须发布；它获得 500 ms drain。Episode 开始间隔至少为 750,000,000 ns。每个 episode 前都把第一个 node 重置为 `0.80`，并在 latency sample 外物化、结算 baseline target。 |
| `I2-progressive-v1` | 复用精确的 I1 source、graph、seed、edit、source-space Region 与 generation lineage。512x512 preview source 是对 2048 source 逐 channel 执行 4x4 box average、只舍入一次到 binary32 后，再经过相同四个 transform；I1 Region 在 preview coordinate 中映射为 `(64*(i mod 4), 64*floor(i/4), 64, 64)`。Final 计算 2048 source。只有第 12 次 edit 的 preview 与 final 是必需 latency result，且必须按此顺序出现；stale output 不得发布。 |
| `B1-immutable-v1` | 包含 immutable job `0..29`；job `n` 使用 source seed `n`、baseline graph、Throughput QoS、weight 1、无 deadline 或 supersession、精确 reservation 证据、canonical trace、已提交 artifact 与按 job index 区分的 golden digest。偶数 job 属于 Graph A，奇数 job 属于 Graph B。测量边界上 harness 同时提供两个有序的 15-job queue，且不会暂停非空 queue；由有界 Host admission 而非 harness 决定驻留多少个 Run。Run cap 1 与 8 是两行独立的必需证据。 |
| `M1-shared-v1` | 在 measured time zero 启动 I1，此后每 750,000,000 ns 重复一次，共精确启动 40 个 episode；同时循环执行精确的 B1 corpus，保留偶数/奇数 Graph 分配、Run cap 8 与持续 offered backlog，共测量 30 秒。两条 stream 共用一个 `ExecutionService`、worker set、ready store、policy binding set 与 `ResourceLedger`；不得用隐藏 pool、重复 ledger 或独立进程承接任一 stream。 |

对 fairness 而言，只要某个 Graph 的 producer 仍有未消费的 offered demand 且没有
暂停提交，该 Graph 就是 *eligible*。这段 workload-level interval 包含等待有界
admission 的时间；它不声明全部 30 个 B1 Run 同时被准入。每个 Graph 内部按递增
job index 提供工作，前一个 job terminal 时 producer 同步提供下一个。M1 启动新的
`0..29` cycle 时不得产生 producer-side gap。

I2 具有必需的 Host-local output path 与条件式 Metal residency 组件。Preview 与
final 都向同一个本地 consumer 两次暴露各自不可变的 CPU `ValueRevisionId`、Host
binding/allocation identity 与 storage byte；两次获取必须复用同一个 binding，不得
发生 CPU copy。存在 `DeviceId(DeviceBackend::Metal, 0)` 时，每个不同
preview/final revision 的第一次 access 可以执行一次精确大小的 Host-to-Metal
transfer，第二次必须复用同一个
device-local residency，且 transfer 与 allocation 都为零。禁止 Metal-to-Host
transfer、filesystem/codec I/O，以及上述两个条件式首次 access 之外的任何 transfer。
没有 Metal 时，只有 device-specific 组件属于预定义 `not-applicable`；Host reuse
与 no-I/O 门禁仍然适用。第 12 次 edit 的 final logical digest 必须等于 I1 edit-12
digest，preview digest 必须等于其自身 fixture golden。

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

Durable-output owner 必须先写入并结算 payload，最后以 atomic publication 提交
manifest。Commit receipt、payload 与 manifest hash 都属于证据；payload digest
必须匹配不可变且按 job index 区分的 golden。

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
- 全部 resource limit 与 Interactive headroom。

v1 resource configuration 是 32 个 CPU slot、1 GiB Host retained memory、
512 MiB Host scratch、65,536 个 ready entry、256 MiB ready byte；Interactive
headroom 为 1 个 CPU slot、64 MiB retained memory、32 MiB scratch、1,024 个
ready entry 与 16 MiB ready byte。Compute I/O 准入上限为 64 个 task 与 256 MiB
计划字节总量。配置 Metal 时，其 device-memory 与 scratch limit 分别为 512 MiB
与 256 MiB。Metal 缺失属于预定义 `not-applicable`，不是零观测。

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

Latency 从 Host admission 前立即开始，到匹配的 current generation 可见时结束。

- I1 final-generation p50/p95/p99 必须分别不超过 50/100/150 ms，且每个 measured
  episode 都必须发布 final generation。
- I2 edit-12 first-preview p50/p95/p99 必须分别不超过 50/75/100 ms；edit-12
  final p95/p99 必须分别不超过 500/1000 ms。两个 endpoint 都必须匹配各自必需的
  logical digest。
- M1 必须满足 I1 绝对 limit，且其 p99 不得超过配对 isolated I1 p99 的 2.0 倍。

被取消的中间 generation 不进入成功 percentile。Accepted-cancel-to-physical-
quiescence duration 作为独立观测保留。

#### Throughput

Throughput 是每秒成功的 logical site-operation，以 MPix-op/s 报告。一个 B1 job
只有在 Run success、所需 artifact commit 与 golden verification 完成后才精确贡献
16,777,216 个 site-operation。其 isolated interval 从两个 measured queue 被提供
前立即开始，到最后一个 manifest commit 与 golden verification 后结束。Candidate
与 reference replicate 按 ordinal 配对：三个 candidate/reference ratio 的中位数
必须至少为 0.95，且每个 ratio 至少为 0.90。

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

- logical output SHA-256；
- canonical artifact-manifest SHA-256；
- 按 job index 区分的 golden SHA-256；以及
- semantic trace fingerprint。

Semantic fingerprint 排除 timestamp、physical worker id、全局 mint 的 id 与原始
sequence number，但保留 run-relative task、action、dependency、terminal-outcome
与必需 resource 事实。Raw physical trace 仍作为证据保留。跨环境带容差比较是
兼容性证据，不能满足这项同环境精确判定。

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
post-quiescent reservation/grant delta。任何权威 dimension 都不得超过冻结 limit。
Isolated 行必须精确结算到 row 前 baseline；M1 shutdown 必须结算到零。

对每个权威 dimension，candidate B1 与 I2 peak 必须不超过已固定同环境 reference
的 105%，同时仍满足绝对 limit。Process RSS 只作为 diagnostic，因为它包含当前
authority 之外的 allocation；它不能替代 ledger/device 证据，也不能免除 settlement。

### 证据按内容寻址并采用 Fail-Closed 规则

每个 measured row 都属于一个 `execution-profile-slo-v1` bundle。Bundle 包含全部
冻结来源、raw sample/event、eligibility window、drop/gap counter、output/artifact/
trace/golden digest 与 commit receipt、transfer/copy/residency 证据、high-water
sample、aggregate input 与 result、独立 verdict，以及所选 reference bundle
digest。Eligibility 表示上文定义的 offered-demand interval。单位、公式、
denominator 定义和 invalidation reason 是 schema field，不能只写在说明文字中。

Reference 与 candidate 必须具有相同 schema、workload id、environment class、
resource limit 与 fixture hash。Reference 不可变且按 digest 选择。未记录的“known
good” build 重跑与 Markdown summary 都不是规范 reference。Raw evidence 必须能够
复算每个 aggregate 与 verdict。

### 后续 Issue 负责固定的证据行

| Issue | 必需 v1 交付 |
| --- | --- |
| #93 | 实现 I1 request/current-generation 与 cancellation/quiescence 观测；发布 isolated latency、waste 与 memory 行，以及必需的 output-correctness 证据。 |
| #94 | 在精确 I1 lineage 上实现 I2；发布 preview/final latency、Host/条件式 Metal residency 与 copy-waste、memory 行，以及必需的 output-correctness 证据。 |
| #95 | 实现 B1 immutable manifest、reservation、canonical trace、artifact commit 与 golden；在 Run cap 1 与 8 下发布 isolated throughput、determinism、zero-fault waste 与 memory 行。 |
| #96 | 把精确 I1 与 B1 fixture 组合为 M1；发布 mixed latency、throughput progress、fairness、waste 与 memory 行。 |

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
