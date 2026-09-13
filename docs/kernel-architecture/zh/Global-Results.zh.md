# 分页全局结果

英文权威文档：[Global-Results.md](../Global-Results.md)。

安装后的 C++ registry 提供 `start_result`，即 dependency protocol 2。
`WorkflowDocument` 的 Result 边携带 `SchemaTemplate`，贯穿 semantic IR 和
execution plan。`OperationPortKind::Result` 区别于普通 Value，不构造虚假的
dense descriptor、零 extent Value 或终结性 RequestRecord 适配器。

## 静态与动态事实

schema 包含 id/version、1..16 个具名 primitive 字段、可选 domain 和有界语义
metadata。每个字段为 packed records，record 有 0..7 个正维度。行数约束为
Fixed、InputAxis、InputElements、FieldRows（前序字段）或 RuntimeCount。
输入派生约束根据 metadata 解析，动态 count 保留到发布；ceil division 和 offset
检查溢出。空集合为零行且没有数据 backing。消费者检查 ResultDescriptor，
不替换既有 Value descriptor，也不动态加载 operation。

ResultBuilder 在创建时固定 ObjectId。语义 key 包含规范 schema 和带长度的
执行 scope。运行时 scope 包含 operation contract、参数、有序输入和 frozen
输入身份。页大小、文件偏移、descriptor revision 和资源限额不进入语义身份。
独立执行的可变 RegionalSource 使用不同 scope；物理选择仍进入物理 plan 身份。

CompleteBundle 在 seal 前隐藏。StablePrefix 和 IndependentChunks 当前提供
按序字段前缀；新范围不可撤销，data/control/validation/descriptor 义务都必须
终结。IndependentChunks 当前支持该按序子集，未提供乱序发布。ResultFinality
是注册算法作者承担的义务，内核不自动生成证明。表示族另加数值和跨字段检查。
descriptor 是固定大小不可变快照，旧 descriptor 不会自动获得新范围权限。

## 阶段和显式 I/O

operation 使用宿主分配的 ResultContinuation，声明 state/workspace 字节和阶段
上限。ResultProgramPhase 只暴露已就绪的 Value fragments、结果对象和 I/O 回复。
poll 返回有界 ResultProgramNeed、ResultPublication 或 ResultValuePublication。
Need 最多 64 项，并遵循所选输出的输入 projection。

读写 plan 保留精确区间授权和存储 owner。临时创建、读、写、扩展为闭集 I/O
动作，由既有 coordinator 在 callback yield 后处理；既有 worker 执行 callback。
structured callback 内直接调用临时 I/O 产生 sticky protocol error；忽略宿主
read、allocation 或 work 失败不能变成成功发布。普通 source callback 保留显式
source I/O 契约。

当前 structured 执行使用 CPU stage，要求配置 managed_resources；支持普通同步
及 dependency-protocol-1 Value 祖先。混合网络的终结性 RequestRecord 输出执行完整 query，
不拆为 Atomic。Result 输出从 ExecutionResult::results 取得，也可用
result_publication 观察。execute_stream 包含具名 Result 输出时要求该 observer，
普通 Value 输出仍使用原 sink。返回的读取窗口持有其授权和字节。
通知按逻辑 ValueRef 去重，等价 step 共享 producer 时仍分别通知。晚加入的 alias
收到当前 descriptor；已观察的 alias 继续收到后续 prefix 和完成通知。首次 observer
失败会停止该 Run 的后续通知。

## 所有权、共享和取消

必需 backing 独立于可选 completed cache。同 Run 的等价 producer 共享 actor；
同 FrozenExecution 的调用加入 context 内 active producer。完成后的索引为弱
引用，只在结果或 owning window 仍存活时复用，不独自保留已完成数据。每个
waiter 获得自己的发布通知，等待发生在 coordinator，不占 worker。

运行时发布绑定有序输入 ObjectId，并强持有最多 16 个直接输入 ResultRef。
因此最终派生结果可能保留整个输入 ancestry 和 backing，这是保守存活策略，
不宣称最小 liveness。所有存活 owner 继续受根预算约束。context 结束后，最后
result/window owner 仍负责释放相应存储。

一个 waiter 的取消或 observer 失败不会取消其他活跃 waiter 的共享 producer。
每个调用声明从具名输出可达的 Result key；共享 producer 的需求根据原始调用及其
取消标志计算，不递归依赖其他 producer token。coordinator 等待 callback 时刷新
需求；任何退出（包括异常）前，都在相同有限资源/work 限额下完成其他 waiter 的共享义务。
最后 waiter 取消会停止 producer；启动失败通知已加入 waiter。退休 producer
不能安装 complete，旧 epoch 不能修改替代条目。后续 operational failure 保留
已认证前缀。没有 detached producer thread。

## 依赖证据和计账

ResultRelation 提供有界 Cartesian、identity、分页 rows、union 和 composition，
明确声明 Exact、Conservative、Unknown。Conservative 组合后不会变为 Exact；
Unknown 的 dirty 查询返回 Unresolved。新 relation 不进入旧 Exact-only
DependencyCertificate。data 与 descriptor 支持独立，空集合仍需要 count/basis/
validation witness。Exact 构造是可信 operation 作者承担的证明义务。

根账本准入 payload、显式对象 metadata、entries、callback queue slots、编码后
磁盘 extent 和 I/O window。新 schema 字符串、嵌套 metadata、actor 容器、语义 key
和共享结果表使用 ResourceAllocator，在分配前预留容量。已编译的静态 template
由 plan 持有，运行时借用。返回的 diagnostics 和 cancellation token 在复制及 context
结束后保留 allocator 根。发布、relation 和 I/O plan 必须属于执行根，拒绝外部根对象。普通 Value 发布对外部存储执行 Referenced 准入并保留
alias；coordinator 的 fragment/Whole 表使用根 allocator，临时 legacy invocation
数组及静态 metadata 副本使用显式 bridge lease。旧 Value/Footprint/session 内部
分配仍属于下述排除范围。
work、已提交 stage 和临时 I/O 累计计费。低限额
返回 ResourceExhausted 并释放普通 owner，未验证的物理清理保留 quarantine。
[管理资源范围](../Managed-Resources.md)不含 allocator 内部 header/control block、
OS cache、驱动、线程栈和旧有未计账 metadata，不构成 RSS 上界。

## 可执行验证

tests/integration/test_result_execution.cpp 仅用安装公共 API 执行
RegionalSource→动态 ID→两个求和消费者；独立整数循环检查 0/37/8192 candidates、
多种页大小、cache-off、frozen 复用、并发取消、启动失败、streaming 通知、有界
失败和最终释放。tests/unit/test_global_results.cpp 检查 descriptor 授权、发布
策略、分页支持和超过 host 预算大小的结果。构建、focused CTest 和安装 consumer
命令见英文文档，可原样执行。

阶段 A 的三条效果 workflow 和复杂表示族另有实现项，本基础测试不代表它们已验收。
