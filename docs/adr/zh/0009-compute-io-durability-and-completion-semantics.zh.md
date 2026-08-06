# ADR 0009：Compute I/O Durability 与完成语义是不同契约

## 状态

作为 Issue #87 的目标契约接受。独立审核已在 Primary 提交
`f8eba94a53fb5bd41250489df88f5d6480baf853` 上通过完整决策，包括全部三项
Codex review 修复。生命周期收口由 Issue/Project 历史与对应 OpenSpec 生命周期
分别记录。

Issue #87 以决策与文档变更的形式接受本 ADR。Issue #88 现在只实现其中的有界
执行边界：唯一 source-private `ComputeIoExecutor` 与一条 staged HP cache-save
垂直路径。它不修改协议 v2 或已安装 ABI，不让当前 Graph/cache writer 变成原子
事务，不把 cache failure 移到 Run publication 之后，也不把当前私有 IPC
`OutputStore` 变成 crash-durable store。后续专项变更必须实现 Run publication
之后的 cache outcome、durable 输出提交、Graph 文档事务和旧输出副作用迁移。

Issue #95 现在实现了一条有意收窄的 source-private B1 手工/release 输出所有者。
`B1OutputStore` 把 Issue #88 executor 与面向精确不可变 B1 artifact 的 rooted fresh-
occurrence、manifest-last/no-replace 事务、类型化 crash-durable receipt 及 leaf-to-root
barrier 组合起来。它不替代私有 IPC delivery store，不新增已安装输出 API，也不完成
本 ADR 中通用 recovery、post-publication cache、Graph 文档与旧输出副作用目标。

在 Primary head `c99c94b56065aee6d456337af8ee0aa45c12e0a1` 上对 Issue #118
进行的后期审核，在其复用的 Issue #88 executor 依赖中发现两条死锁：同一 worker
提交后等待 completion，以及已准入 lazy factory 对同一 executor 发起 shutdown。
这两项修复是 Issue #118 结算所需的 Issue #88 mechanism hardening；它不会把
executor 所有权或 policy authority 转移给 OpenEXR V-15 change。

## 背景

PhotoSpider 已经有若干有效的完成与持久化机制，但每个机制回答的问题都不同：

- `ComputeRun::Succeeded` 表示经过校验的 Graph/RT 发布或经过校验的 no-op 赢得
  Run 终态仲裁。仅 provider 返回并不足够。
- `GraphCacheService` 直接写入配置的图像与元数据路径。一个缓存条目不是事务性
  发布；当前 product commit policy 还可能在可见 Graph 发布之前，因为延迟缓存
  持久化失败而令 Run 失败。
- `ImageArtifactCodec` 与 `CacheMetadataCodec` 拥有表示转换，不拥有目录创建、
  路径权威、原子替换、重试、可见性或 durability。
- Graph 文档加载会在替换内存 Graph 前校验 detached definition。当前 YAML 保存
  在打开前完成 emit，但直接写目标流；打开后的失败可能留下新建、截断或部分
  文档。
- 协议 v2 daemon 作业是有界的进程内 polling 记录。优雅关闭会排空已接受工作，
  但 daemon 崩溃会丢失 queued、running 与 terminal 记录。
- 私有 IPC `OutputStore` 写入同 owner、mode `0600` 的 stage，调用文件
  `fsync`，执行禁止覆盖的原子 rename，校验 identity，然后发布内存 lease
  记录。它不同步包含目录，也不持久化记录/索引；TTL/lease 清理还会有意删除
  制品。
- 旧 `io/save` 操作在 provider 执行期间调用 `cv::imwrite`。其用户选择路径可能
  在包围它的 Run 提交前可见；若取消或其他终态竞争者随后获胜，也无法回滚。

如果把所有这些状态都称为“完成”或“已保存”，worker pool 的选择就会意外定义
事务所有权。因此 Issue #87 在 Issue #88 引入有界 compute-I/O 执行之前冻结了
权威与失败顺序。

本 ADR 遵循：

- [ADR 0003](0003-process-owned-execution-resources.zh.md)，它把物理执行资源分配
  给进程；
- [ADR 0006](0006-kernel-documentation-separates-facts-decisions-targets-and-status.zh.md)，
  它禁止把目标行为当作当前事实；
- [ADR 0007](0007-compute-runs-and-process-execution-have-separate-owners.zh.md)，
  它分配 Run 终态与可见 Graph 提交权威；以及
- [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.zh.md)，
  它拆分逻辑 Value、runtime binding、artifact identity 与持久化层。

## 决策

### 完成语义是类型化偏序

目标契约区分下列可观察事实：

| 事实 | 所有者与含义 |
| --- | --- |
| `RequestAccepted` | 某个所有者准入请求并返回其稳定请求 identity。 |
| `OperationReturned` | provider 或 codec 调用返回且没有即时错误；原生工作仍可能 pending。 |
| `ValueReady` | 消费值所需的所有 producer fence 均已成功到达终态。 |
| `RunTerminal` | `ComputeRun` 仲裁器精确发布一次成功、失败或取消。 |
| `ResultAvailable` | 可以取得受保留快照或 delivery lease。 |
| `OutputCommitted` | 输出权威已越过请求的原子可见性与 durability 点，并能返回稳定回执。 |
| `OutputCommitFailed` | 输出事务报告类型化的 encode、暂存或请求 durability 提交失败；它没有返回声称达到请求 durability 的回执。 |
| `GraphDocumentSaved` | 独立文档事务已发布一个版本并报告实际 durability。 |
| `ResponseObserved` | client 收到确认；响应丢失不会撤销更早的服务端转换。 |

这些事实构成显式偏序，不是别名。`OperationReturned` 不暗示 `ValueReady`；就绪
不暗示 Run 成功；Run 成功也不暗示缓存保存、输出提交、Graph 文档保存、daemon
结果可用或响应观察。

合法 Run 分支具有不同偏序：

- 成功产值 Run 依次观察到 operation 成功返回、producer fence 成功完成、
  `ValueReady`、经过校验的 Graph/RT 发布，随后才是
  `RunTerminal(Succeeded)`；
- operation、readiness、dependency 或 Run terminal 之前的 `ComputeRun`
  result-commit failure 可以发布其类型化失败事实，随后直接进入
  `RunTerminal(Failed)`，不得伪造 `ValueReady` 或 `OutputCommitted`。这里的
  result-commit failure 只限该 Run 的 Graph/RT validation、publication 或
  commit resolution，不包含后续 `OutputStore` 事务；
- 赢得 Run 仲裁的 cancellation 进入 `RunTerminal(Cancelled)`。迟到或 stale 的
  provider/fence completion 只执行清理，不能发布 Value 或 durable-output
  receipt；若某项输出事务在之后的 cancellation 前已经独立提交，其回执继续对该
  输出事务保持权威；以及
- 已准入的 empty-plan、zero-work 或其他经过校验的 no-op 可以从 no-work 校验直接
  进入 `RunTerminal(Succeeded)`。复用已有完整结果不得伪造新的
  `OperationReturned`、`ValueReady` 事实或 durable-output receipt。

`ComputeRun::Succeeded` 保留 ADR 0007 的含义：经过校验的 Graph/RT 发布或经过
校验的 no-op 获胜。未来 `compute-and-persist` 或 export API 是具名组合操作。
它暴露两种结果，只有承诺的每种结果都成功时才成功；它不扩展 Run 终态。
目标中的 Run 后置 cache、codec 与 output 工作拥有自身类型化 outcome，不得延迟
或改写已经发布的 Run 终态。
Run terminal 之后的输出失败发布 `OutputCommitFailed`，而不是
`RunTerminal(Failed)`，既不产生也不撤销 `ValueReady`。调用方或 daemon 可以
报告组合 request failure，但必须保留下层 Run terminal 与 output、document、
cache/codec 和 response 事实；不能把聚合结果反投射回 Run state。

### 每类持久化只有一个权威

| 持久化类别 | 权威 | 契约 |
| --- | --- | --- |
| Graph 文档 | Graph 文档事务所有者 | 用户编写的拓扑/配置；独立于 Run 与缓存版本化。 |
| 缓存制品 | 缓存 policy/service | 可重建加速；缺失或失败不是用户输出事实。 |
| Codec 结果 | 调用操作 | 只做表示转换；没有事务或 durability 策略。 |
| Daemon 作业/响应 | Daemon registry/transport | 进程级控制与交付状态，不是 durable 存储。 |
| 用户可见 compute 输出 | `OutputStore` 事务 | 由稳定 commit id 与回执标识的原子已提交输出。 |

当前私有 IPC `OutputStore` 是受保护的交付 store，尚不是目标 durable 权威。后续
实现可以演进它，也可以有意引入改名后的 durable owner，但必须只留下一个输出
提交权威。Delivery lease 与 TTL 清理和 durable retention 保持分离。

缓存路径、daemon artifact path、delivery id、Run id、output commit id 与 Graph
文档版本是不同 identity。payload 字节相同也不能让它们互换。

### 失败传播遵循权威

必需的源资源获取、decode、producer readiness 与执行可以令 Run 失败，因为无法
产生有效暂存值。普通缓存缺失、不兼容、损坏或 I/O 失败属于结构化缓存诊断；
允许从权威来源重新计算时，它变成 miss。安全策略违规与资源耗尽保持类型化失败。

目标把缓存写入移动到可见 Graph 发布之后，或移入独立缓存持久化阶段。缓存写入
失败随后只记录缓存失败，不回滚或替换已经成功的 Run 或 output commit。

输出 encode、暂存或 durable commit 失败属于输出事务，并报告
`OutputCommitFailed`，且不产生声称达到请求 durability 的
`OutputCommitted` 回执。Graph 文档保存失败属于保存事务。Cache write/codec
失败属于 cache policy 或调用操作；daemon 响应丢失只改变 client 观察。这些失败
都不延迟、回滚或改写此前发布的 Run 终态。

当前 product 与目标不同：延迟缓存持久化可能在可见 Graph 发布前失败。这是记录
在案的迁移缺口，不是缓存具有输出权威的证据。

旧 `io/save` provider callback 是另一个迁移例外。目标 provider 工作产生暂存
output intent/value；只有 `OutputStore` 编排能在 Run 结果已知后发布调用方可见
输出。不得扩大直接副作用路径，也不得把它当作 durable commit 表面。

### OutputStore 使用 manifest-last 幂等提交

每个目标输出事务都在传输响应可能产生歧义之前获得稳定 `OutputCommitId`。它
绑定一个 namespace、输出槽位、表示 descriptor、内容 identity、已提交版本与
保留策略。

权威执行下列协议：

1. 校验 rooted namespace、请求的 durability、quota、descriptor、内容 identity
   与 commit id；
2. 协调幂等性：相同已提交 identity 返回原回执，同一键配不同内容则拒绝；
3. 为不可变 payload/chunk 数据、元数据与每个事务自有临时文件创建私有同 root
   stage；
4. 完整写入每个 payload 和 metadata 文件，同步每个文件，随后重新校验其精确
   长度、digest、文件系统 identity、`OutputCommitId`、已提交
   version/generation 与内容绑定。Manifest 发布前的任何失败都不会留下已发布
   manifest 或 receipt；
5. 把完整 canonical manifest 编码到唯一私有 stage，写完全部 manifest 字节，
   并校验已存 canonical content、精确引用集合、payload 长度/digest，以及
   `OutputCommitId`/version/generation/content 绑定。在发布前同步 manifest
   文件本身；
6. 使用平台支持的原子 no-replace 操作发布最终 manifest/commit record，使其成为
   唯一多文件可见性点，随后重新校验已发布 identity；
7. 对于 crash durability，为本事务创建、rename 或修改过的每一级目录执行
   durability barrier，顺序从叶目录到配置的 durability root。每个屏障使用
   directory `fsync` 或有文档说明的平台等价机制；
8. 只有在请求 durability class 要求的每个屏障均成功后，才持久化并返回
   `OutputCommitReceipt`；以及
9. 恢复时识别已提交 manifest，重建 commit index，并保守删除或隔离未完成
   stage 和 orphan。

具体 transaction 会保留已打开的所选 canonical root descriptor 与每个全新 private
slot descriptor；所选 root pathname 是 evidence，不是持续 mutation authority。
Creation、file access、publication、barrier、revalidation 与 cleanup 始终采用
descriptor-relative 操作，并验证预期 filesystem identity。因此 root path replacement
或 symlink substitution 会使最终 binding 失败，而不会重定向写入。

源码私有的 B1 实现会在 store 生命周期内取得所选 root 的 nonblocking advisory
exclusive lock，并创建 mode-`0700` 的同 root staging anchor 与一个 private child slot。
其 namespace contract 只覆盖一个协作式 store owner：所有协作进程/线程都必须遵守该
lock，并把 `.b1-staging-*` 与 `occurrence-*` name 保留给这个 owner。该 lock 不是防御
任意不协作 same-UID actor 的安全边界。对每次 `mkdirat` → `openat` handoff，它会先记录
no-follow named directory identity，在此之前不修改 child 内容，然后要求打开的 descriptor
具有完全相同的 identity。如果 anchor handoff 在 transaction guard 建立前失败，实现会保留
含义不确定的当前 anchor name，传播失败，且不声称可重试。如果 slot handoff 在 guard 激活后
失败，而 replacement residue 阻止精确 guarded cleanup，则会 fail-stop。Payload 与 manifest
task 只修改已验证的 private slot。两个 accepted charge 均结算后，完整 directory 会通过一次
atomic no-replace directory rename 发布到不可变 occurrence name（Darwin 使用
`RENAME_EXCL`，Linux 使用 `RENAME_NOREPLACE`），随后完成 source anchor 与 destination
root barrier，并最终重验 descriptor/name binding。实现不会先创建 public occurrence，再重新
打开它进行写入。

Allocation-free transaction guard 会在 anchor、slot、payload、private manifest 与
published manifest 逐一归属事务时记录其精确 identity。若后续 factory、observation、
wait、publication 或 receipt 工作失败，guard 会先 cancel 并等待每个 accepted Compute
I/O task，并证明其精确 charge 已退休。Cleanup 是严格操作而非 best effort：每个存在的
name 都必须通过两次 descriptor-relative 的已记录 type/identity 检查；每次
`unlinkat`/`rmdir` 结果、随后 absence 与 parent-directory sync 都必须成功。POSIX 不会把
最终 identity 检查与后续按 name 删除合并成一个原子的 identity-selected 操作。在协作式
exclusive-owner 前提下，不会有 actor 在这段间隔修改 reserved name；任一次检查检测到
replacement 都会在删除前保留它并 fail-stop。最终检查之后由不协作 same-UID actor 发起的
mutation 不在本 contract 内，设计也不声称永远不会删除这种 replacement。Extra leaf、
type/identity 漂移、`EIO`/`EROFS`、非空 directory 或无法证明 absence 都会 fail-stop。
只有在该前提内完成 checked removal 并观察到 absence 后，原 commit identity 才保持可重试。

回执标识 commit、descriptor/content、namespace、version 与达到的 durability。
它不是可变 cache 或 staging path。默认策略绝不覆盖已提交输出；替换使用显式
新 version/commit identity。

实际达到的 durability 是类型化的。显式请求 atomic-visible 的事务只有在
no-replace manifest 发布和 identity 校验后，才能返回仅声明原子可见性的回执。
只有 manifest 文件及其引用的全部文件均完成同步，且从叶目录到 root 的完整目录
屏障成功后，才能得到 crash-durable receipt。无法提供所需 file synchronization、
directory barrier 或 atomic no-replace publication 的平台/文件系统会报告类型化
unsupported 或事务失败，绝不会把较弱结果标成 crash durable。若原子可见后 crash
durability 失败，不产生 crash-durable receipt；使用相同 commit identity 的重试会
以幂等方式协调该状态。

交付采用 at-least-once，提交具有幂等性。提交点之后响应丢失时，使用同一 commit
id 查询或重试。store 返回同一回执、继续可恢复 pending 事务，或报告类型化冲突/
失败。它不声称 exactly-once 传输，也不为同一键创建第二个已提交输出。

manifest 提交点之前，取消或失败会撤销发布许可。迟到 codec/I/O 工作只能完成
私有 stage，然后进行 identity-safe 清理。提交点之后，取消对于该事务是 no-op，
不能重标记或删除已提交输出。恢复会从已提交 manifest 重建回执，并保守清理或
隔离未完成 stage。

### Graph 文档保存是独立版本化事务

Graph 文档包含用户编写的拓扑/配置，并排除 runtime 输出、缓存条目、原生
binding、delivery lease 与输出回执。目标保存事务：

1. 捕获一个 Graph revision 与预期 document version；
2. 在改变目标前序列化并校验完整 detached document；
3. 在调用方授权 root 下解析归一化目标；
4. 写入并校验私有同目录 stage；
5. 按请求 durability 同步；
6. 只原子替换预期 document version；
7. 请求 crash durability 时同步目录；以及
8. 返回带 document/version identity 与达到的 durability 的类型化保存回执。

过期预期版本失败，不覆盖并发保存。无法提供所请求原子替换或 durability 的
平台/文件系统会报告类型化 unsupported capability。调用方可以显式请求较弱的
进程可见等级，但实现绝不会静默地把直接 stream close 标成 durable。

Graph 文档保存绝不是 `ComputeRun` 的隐式阶段。

### Daemon 状态保持为传输状态

daemon 可以拥有 accepted、queued、running、terminal、result-available 与 response
状态。作业只有在其 result mode 承诺的工作完成后才到达终态；但除非存在独立
输出回执，其终态名称不暗示 crash-durable 输出。

优雅关闭继续停止准入并排空已接受作业。崩溃可能丢失进程内 registry。恢复使用
稳定领域 identity 与 `OutputStore`；不会把 daemon registry 提升为 write-ahead
log，也不会自动重放没有幂等键的 mutation。

Issue #87 不修改协议 v2。后续版本化协议可以在远端调用方需要 durable commit
时暴露独立输出事务与回执。

### ComputeIoExecutor 拥有有界机制，不拥有策略

Issue #88 新增唯一 process-owned `ComputeIoExecutor` mechanism，用于有界 cache、
asset 与 codec 子工作。准入会在 lazy payload construction 或副作用之前，同时
原子覆盖 task 数与 estimated retained bytes。每项已接受任务都会保留
Run/transaction lifetime token，并返回 `Succeeded`、`Failed` 或 `Cancelled`
typed completion。Cancellation、callback failure、late return 与 graceful shutdown
都会恰好一次释放该 token 与两项账本。CPU compute worker 不能同步等待 completion。

executor 还负责签发归属 proof。它在与每次 admission decision 相同的 mutex 下签发
不可变 event，其中包含单调非零 sequence、精确 task/byte charge delta、类型化 decision
与结果 process-global snapshot；又在与 settlement release 相同的 mutex 下签发第二个
不可变 event，关联该 admission，并携带精确 released delta 与结果 snapshot。Rejected
offer 的 delta 为零。Snapshot 可以包含无关并发工作，可用于 limit/high-water 验证，
但不能替代该 task 自己的 charge/release event。当 consumer 只观察部分 process work
时，event sequence 允许存在 gap。

唯一 I/O worker 不能向自身 owning executor 准入另一个任务：准入仍开放时，该调用
会在改变任一 budget 或 lazy factory 前返回 inactive `InvalidRequest`。Owning worker
上的 completion wait 可以复制已经 terminal 的 fact，但会在 condition-variable wait 前
拒绝 nonterminal fact。这些 guard 比较准确 executor identity，因此一个 I/O callback
仍可向另一套独立 executor 提交并等待。

Lazy factory execution 由无分配、异常安全的 thread-local stack 跟踪。凡 shutdown
目标仍存在于该 stack 中，调用都会在改变 shutdown state 或 join 前失败，其中包括
间接 `A factory -> B factory -> A shutdown` cycle；无关 executor shutdown 仍合法。
外部 shutdown 仍会线性化 admission stop，等待每个已计费 factory 返回或抛出；若返回的
submission 在 publication race 中落败，则会在 callback entry 前取消；若异常逃逸，则精确
回滚一次；随后 drain 已发布工作，最后才完成 worker join。

首条生产路径是 staged HP cache-save callback。Graph-state policy owner 选择
eligibility、path、precision、codec 与 publication 前的排序，然后等待 typed
result；executor 不改变 Graph state，也不选择 visibility point。当前 codec API
只暴露一个不可拆分的 I/O-facing call，因此该垂直路径会在 I/O worker 上运行整个
调用。这不表示 CPU-heavy codec work 可以普遍归入该 worker；后续拆分后的 codec
contract 必须把独立准入的 CPU phase 送回 CPU execution domain。

Graph 文档事务、daemon socket/polling 与 `OutputStore` 校验、提交、回执、保留
和恢复仍属于各自领域 owner。这些 owner 可以提交有界字节传输或 codec 子工作，
但执行器绝不选择路径、重试、覆盖、幂等、保留、提交或 durability 策略。

当 domain owner 选择在 capacity rejection 后重新 offer 时，该 policy 必须声明有限
且确定性的 attempt bound 与类型化 terminal result。它必须在遭拒 offer 之间保持
logical task identity 与 charge，不能从 elapsed time 或 polling cadence 派生终止条件，
并且在耗尽 bound 时释放 private staging authority。Executor 仍然只暴露类型化
admission，不决定该 policy。

拒绝让每种文件系统与 socket 操作都进入一个通用 pool，因为这会组合无关生命
周期，并让 worker 机制成为意外的事务所有者。

### 安全与 durability 能力是显式的

持久化 owner 在调用方授权 root 下解析归一化路径，以 no-follow 方式打开并保留这些
root/slot directory authority，防止 symlink escape，通过 descriptor-relative
operation 创建和修改私有同目录 stage，校验文件系统 identity 与最终 path-to-
descriptor binding，并统计 in-flight 与 retained quota。不受信任的 plugin/codec
只能取得 stage access，绝不取得发布权威。

回执至少区分：

- 进程可见的原子发布；以及
- 完成每个请求且受支持的数据/目录屏障的 crash-durable commit。

不得假设 file sync、directory sync、atomic rename、远程文件系统行为、硬件写
缓存与平台支持彼此等价。无法支持的请求保证显式失败，而不是返回比实现能够
证明的内容更强的标签。

## 结果

- 调用方看到更多显式状态，但不会再把缓存保存、daemon 确认或文件 rename 误当
  成 durable 用户输出。
- 成功 Run 可以合理地没有可选缓存，或其可选缓存失败。结构化缓存诊断与重新
  计算保持正确性。
- durable 输出需要保留幂等元数据、manifest 恢复、目录屏障与显式垃圾回收；
  这会带来存储和同步延迟成本。
- delivery lease 可以过期而不删除 durable 输出。Retention 成为显式输出策略，
  不再是 IPC 副作用。
- Graph 文档保存获得乐观版本控制与类型化 durability 结果，但继续与 compute
  频率和 runtime 状态独立。
- 现有 `io/save` 操作、同步 cache administration/load、直接 YAML writer 和私有
  IPC store 仍是当前事实与记录在案的迁移缺口。Staged HP cache-save 垂直路径
  现已使用有界 executor，但这没有让它变成 atomic 或 durable。
- 长期测试会验证有界准入、精确 cancellation/shutdown settlement、failure 保留，
  以及 cache codec 阻塞期间的 CPU progress。后续 durability 工作还必须验证
  manifest-last visibility、幂等歧义恢复、recovery、durability 能力与过期文档
  version。Issue 专用 scan 或编排不进入 CTest/CI。

## 被拒绝的替代方案

### 让 `ComputeRun::Succeeded` 表示所有内容均已持久化

拒绝，因为缓存、Graph 状态、用户编写的文档与调用方选择输出拥有不同权威、
保留、重试与失败时机。

### 把缓存或带 lease 的 daemon 制品当作 durable 输出

拒绝，因为两者都可被有意淘汰、由 backend 拥有，而且当前都没有持久提交回执
或恢复索引。

### 因为 rename 原子而声称 exactly-once

拒绝，因为响应交付可能有歧义，多文件输出需要独立提交标记，而且原子可见性
不是文件系统 durability。

### 把 Graph 文档保存放进每个 Run 提交

拒绝，因为 runtime 输出不是用户编写的 Graph 状态，而且 Run 频率不能在
graph-state 提交中制造过期文档覆盖或存储延迟。

### 让所有 I/O 都经过一个通用执行器

拒绝，因为执行器准入是机制，而路径、事务、重试、覆盖、提交、保留与 durability
是领域策略。

### 静默降级无法支持的 durability

拒绝，因为成功但错误标记的回执比类型化 unsupported-capability 失败更糟。

## 与当前事实和演进目标的关系

当前行为继续由下列文档作为权威来源：

- [内核数据模型](../../kernel-architecture/zh/Data-Model.zh.md)；
- [ImageBuffer 内存契约](../../kernel-architecture/zh/ImageBuffer-Memory-Contract.zh.md)；
- [Compute 边界](../../kernel-architecture/zh/Compute-Boundaries.zh.md)；
- [Compute 流程](../../kernel-architecture/zh/Compute-Flow.zh.md)；
- [策略与执行架构](../../kernel-architecture/zh/Policy-and-Execution-Architecture.zh.md)；
- [内核术语](../../kernel-architecture/zh/Terminology.zh.md)；以及
- [内核缓存模型](../../kernel-architecture/zh/Cache-Model.zh.md)。

已接受目标与依赖顺序位于
[内核演进路线图](../../roadmap/zh/Kernel-Evolution.zh.md)和 OpenSpec change
`decide-compute-io-durability-and-completion-semantics`。Issue/Project 历史与
OpenSpec 生命周期分别记录交付收口。
