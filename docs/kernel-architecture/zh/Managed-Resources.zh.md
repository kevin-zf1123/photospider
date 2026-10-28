# 托管资源

英文权威文档：[Managed-Resources.md](../Managed-Resources.md)。

## 范围与所有权

每个 `ExecutionContext` 拥有一个 `ResourceBudget`。`ExecutionContextConfig::managed_resources` 提供可选上限，`resource_budget()` 返回同一资源根的 handle。Reservation 和导出存储可以在 context 销毁后继续存活。计量覆盖已申报容量与已接入计量的分配，不测量进程 RSS、线程栈、未托管的分配器开销或普通 `shared_ptr` 控制块、驱动私有内存及 provider 拥有的不透明 kernel context。

独立维度为 Host、Device、Shared、Metadata、Referenced、Entries、Queue 和 Payload。Host 包含 Metadata 与 Shared，Device 包含 Shared，重叠计数不能相加。实际 Payload 上限取 `maximum_live_bytes` 与配置的 Payload 容量的较小值。Context 要求正的 `maximum_live_bytes`。

`ResourceLease` 的副本共享同一 reservation。准入与增长是原子的，容量不足立即返回 `ResourceExhausted`，不等待其他 owner。容量持续计费到最后一个 lease 释放。缩减要求对应存储已经释放或其分配已放弃。`ResourceBudget::consume` 原子准入累计 work 与 stage 计数，已发出的 work 不退款。

## Plan 与准备

编译的结构上限独立于执行资源根。`PlanningOptions` 默认允许 4,194,304 个 task、4,194,304 条物理表总记录、16,777,216 条依赖引用和 268,435,456 个 work 单位。Static planner 在保留 call lowering 时检查预计合并记录数，共享 incoming tile 只计一次，追加 dense import 记录前也先准入。Local operation graph 使用单独的验证上限，因为重复端口可以合并为更少的全局记录。最终验证按请求的全局上限检查发布的程序。验证耗尽保持 `ResourceExhausted`，custom planner 同样适用。

`ExecutionPlan` 副本共享完整的不可变程序、源 IR 和静态 operation/resource owner。输出选择在重建闭包与派生表前复制私有存储。`prepare` 保留这个共享对象；编译器拥有的表和不可变资源 backing 保持原有所有权与计量来源。

`prepare` 在构造 `PreparedPlan` 状态及其 layout、kernel、read、output-port 和 block-work 表前准入 Host/Metadata 容量。每个 prepared kernel 包装对象与 module/device owner 记录分别保留自己的 lease。临时 C++ read/write/output query 数组在分配前准入。operation 名称、输入 metadata、标量参数和编码后的 task 参数借用不可变的准备阶段 owner 或 Plan owner，准备阶段不复制这些容器。输入与输出 schema 共享不可变静态 owner。C module 桥接目录在准备阶段的资源根下使用 `ResourceAllocator`。算子的不透明 prepared context 由 provider 拥有，ABI 8 不申报其分配大小。

PreparedState 与 kernel 包装对象、保留 operation/device 的 owner 包装对象、准备阶段 view 数组和全部 PreparedPlan 表容量均提交实际 host 字节数。Native 容量表提交数组存储，其内嵌对象已计入 PreparedState。PreparedPlan 或 PreparedKernel 的副本共享 owner，不重复提交计数。独立准备分别拥有自己的表。准备中途失败会退休每项已完成分配，并保留峰值。Query 栈对象和固定预留余量不计入堆分配。

Native 准备查询实际分配容量并保留容量表。Device 对象与 native kernel 包装对象向 context 资源根申报托管 metadata。驱动内部分配不在已申报容量模型内。

## Context、Run 与 block 准入

Context 构造时预留 CPU queue 容量与 worker metadata，Queue 容量保持到 context state 销毁。GPU 提交使用 context 级 credit 与各 Run 拥有的 completion slot，没有 context GPU work FIFO reservation。独立的 CPU/GPU 准入链表使用 frame 内嵌 waiter，不分配 queue node；context 对象包含其锁和首尾指针，frame waiter 属于栈状态。

Context 对象、CPU FIFO、worker 数组和 Run 目录在该 reservation 下提交实际 host 字节数。Worker shutdown 后数组继续保留，直到 context state 销毁。Context 销毁时释放 Run 目录并退还其实际字节，即使 PreparedPlan 仍保留已关闭的 context。直接构造的内部 worker pool 仅在传入的 parent lease 下计量数组，排除其栈对象。构造中途失败会在 worker shutdown 后退休已完成的数组分配。

`submit` 在开始执行前准入 Run 与最大 frame 的簿记容量。绑定预检使用计入预算的 binding 指针目录，检查名称与 schema、捕获 Result coverage 并保留 backing owner，不复制 binding 名称。外部输入存储按资源根内的存活 owner 去重后计入 Referenced。该根已经拥有的存储保持原有计费。Device 和 Shared binding 还必须符合 prepared device 与编译后的 layout 契约。

Run 与 value-identity owner 对象、保留的 input-identity 和 delivery-ring 数组、诊断状态与有界 trace 数组也提交实际 metadata 字节数。清空仍保留容量的 vector 不会退还其容量计数。诊断状态拥有独立 lease，可通过 observation 或导出存储 owner 在 Run 与 context 销毁后继续存活。副本共享同一份计费；在其他线程释放时更新原资源根。

输入 tile、identity 和 canonical-owner 目录在 submit 与控制流边转移时携带其分配计数。Frame 接管 tile 和 identity 数组；交接用 canonical 目录保持到 frame 构造结束。每个 frame 独立计量自己的 canonical 目录、hold 计数器、work slot、lane view 和固定 lane completion-message 存储。调度器的 task/call 数组、ready queue、submission ID 与 skipped ID 按已分配容量计数。临时 root 标记数组在准入后退休。Frame 和调度器栈对象不计入堆分配。每个数组在成功分配后 commit，在销毁后 withdraw，构造中途失败同样适用。这些事件保留完整 parent reservation，直到 Run 清理时缩减临时容量。

输入绑定与 frame 导出为每个物理 tile 使用临时 TileView。其 Region、stride 和 origin 数组在 Run reservation 下提交实际容量；输入绑定只分配 Region，并复用它检查元素数。每个 Run 为最大 rank 8 的 view 预留 256 字节，在 task issue 前逐个构造。栈 owner 不计入堆分配。发布过程将保留的 metadata 复制到 Result 拥有的存储；进入下一个 tile 或缩减 frame 容量前，先销毁临时数组，再退还其计数。构造中途失败遵循相同顺序。

每次 block activation 原子预留全部输出容量，以及各 backend、各 domain 的最大 task scratch 乘以该 backend 的活动 lane 数。Native reservation 使用查询后的取整容量。分配将 reservation 分成 storage lease；block 退出时释放未使用容量。导出 Result 保留其 storage lease。因此，只要先前导出的 Result 仍存活，后续 Run 就可能准入失败；释放该 Result 后可以成功。

Run 不展开依赖，也不重新选择 placement。`maximum_block_activations` 默认是 1,048,576，`maximum_work` 默认是 268,435,456。准备阶段计算各 block 的簿记与 COPY work。每次 activation 进入前，从 Run 上限与 context 根扣除该费用；空循环也有正的费用。Legacy invocation 8 的数值循环继续使用 prepared contract，没有 work-service 回调。显式启用的 typed CPU invocation 9 借用本次 Run 的 work/stage 服务，各数值段在计算前准入；`work` 包含声明的算术与 limb 单位，`stages` 包含显式精化 attempt。

Coordinator 与 invocation 9 callback 共用同步的 Run admission。先检查各 Run 独立的 `maximum_work`、`maximum_refinement_stages`，再向共享 Context root 原子准入 `{work, stages}`，成功后才提交两维 Run 计数。拒绝不增加任一维度，按剩余额度检查 uint64 溢出。Context root 跨 Run 累计成功准入量；一次拒绝不会永久锁定 root 失败。每次 submit 创建新的 Run 账本与 sticky Run failure，prepared context 不持有这些计数。

`ExecutionDiagnostics::issued_work` 表示本 Run 已成功准入的 coordinator/invocation 9 工作，不等于 CPU 指令数，也不包含 legacy invocation 8 内部数值工作。取消、task 失败与 drain 不退还已准入 work/stage。Host 预算拒绝保存 `Resource`/`Run` 归因及已知 node/task；coordinator 在有界安全点停止后续准入，排空已接受工作。拒绝与 issue 竞争时，可能留下一个已接受的在途 invocation。取消保持终态优先级；预算失败不通过设置取消标志表达。CPU 服务仅借用至同步返回，不得存入 prepared state 或跨 Run 复用。

Task 状态、completion slot、有界失败消息与 retirement 投递记录在 issue 前预留。非法或重复 completion key 以 `Internal` 和 `MalformedEnvelope` 中止，不分配内存，也不结算其他 submission。已接受的工作持续保留 invocation、view 与 storage，直到 completion 确认所有访问结束。取消停止新工作，不提前释放已接受工作的存储。

调度器失败缓冲区与 Run 的 block/terminal 诊断缓冲区提交实际 string 容量，包含终止符。已结算状态向预留的 Run 存储复制最多 256 字节，保留结构化错误详情，不转移调度器的存储所有权，也不分配内存。每个退休记录在排队与投递期间携带其消息分配 owner。投递线程先释放记录，再报告结算完成；后续 frame 不会与已结算记录的消息分配重叠。Observer 副本与 `join()` 返回的诊断由调用者拥有。调用者的 `join()` 复制失败时，Run 中已锁定的诊断保持完整。

`RunHandle::observation()` 保留独立计数器，不保留 Run 或输出 Result。它公开 block 拒绝详情、输出分配次数、head 等待时间，以及输出/scratch backing 各域的 reserved/live 当前值和峰值。导出 owner 在 Run/context 销毁后释放时仍会更新计数。分域 backing 记录排除 host metadata；资源根统计中的 `live` 仍表示已准入容量。完整字段与时序语义见[并行执行](Parallel-Execution-Model.zh.md#诊断与检查)。

`ResourceStatistics::allocated_metadata_bytes` 和 `peak_allocated_metadata_bytes` 独立统计已接入分配事件的实际提交字节。当前接入成功构造的 lease 对象、托管 allocator block、CpuStorage owner、外部引用 owner 对象、block 分配簿记及上文所述准备阶段 owner 和表。Metadata 模式提交整个请求的分配，包含对齐头；Payload 模式仅将对齐头计入 metadata。单纯 reservation 只提交其簿记对象。别名不重复计费，其他线程释放时更新原资源根。存储释放后可显式 withdraw，再缩减容量；shrink 不能移除仍支撑已提交 metadata 的容量。Quarantine 阻止 withdraw；验证完成并结算后恢复正常 withdraw 与退休。Lease 簿记对象本身按正常生命周期退休。

`ExecutionDiagnostics::metadata` 报告归属每个 Run 的 reserved/allocated metadata 当前值和峰值。Submit bootstrap、coordinator 及资源根匹配的嵌套分配继承该 Run observation。Lease 弱引用原计数 owner，因此跨线程释放及导出存储继续更新原 observation，不保留 Run，也不形成所有权环。Context/PreparedPlan owner 与预先存在的 input storage 保持原归属，并发 Run 不重复计费；不同资源根不继承活动 Run 的归属。现有根 `live`/`peak` 继续表示预留容量；allocated 计数排除 allocator 私有开销与 RSS。

## 存储与 Result

CPU `BufferAllocator` 继续保证新分配字节为零。Linux 的普通分配从 1 MiB、macOS 从 32 MiB 起，对请求对齐能够整除系统页大小的分配使用私有匿名映射；其他请求保留对齐分配及清零。映射按页取整的长度仅用于释放，逻辑容量仍按请求字节数计量。所有权、失败观察、预算准入及释放计量保持原有规则。零页按需提供，因此分配成功不表示所有页面已驻留或已承担首次写入缺页成本。Linux 对至少 32 MiB 分配内部完整的 2 MiB 对齐范围额外请求 `MADV_HUGEPAGE`；提示失败不影响正确性，不修改系统设置，也不保证驻留或性能。原生 Shared/Device 分配路径保留各自策略。 当块恰有一个 CPU/Host 任务和一个已授予 CPU 执行通道，声明的 Host scratch 在 [1 MiB,32 MiB) 内，且 Host 输出字节数至少为 scratch 的四分之一时，Linux 通常将该块 Host 输出与 scratch 的匿名映射门槛设为 4 MiB。如果 Host scratch 大小为 2 MiB 的整数倍，且块的 Host 输出至少达到该 scratch 的一半，则该 scratch 保留普通映射门槛和既有的整页工作区路径；在 4 MiB 以下，该例外只涉及恰好 2 MiB 的 scratch。低于门槛的请求采用对齐堆分配并完整清零，宿主分配器可能复用已释放的空间；本策略不保证物理地址或页面复用。公开 allocator 调用、输出比例较低的块及其他 placement 保留普通门槛。门控只使用声明容量，不检查 payload 实际使用量。堆分配仍在准入后完整清零，生命周期和计量不变；该有界策略不新增保留缓存，也不改变 macOS 门槛。 对恰有一个 CPU/Host 任务、一个已授予 CPU 执行通道的块，当声明的 Host 输出字节数至少为 scratch 容量的一半时，Linux 还可对 [2 MiB,32 MiB) 的 Host scratch 内部完整对齐范围提示大页。提示成功后，在缓冲区交给 worker 前通过 volatile 指针进行至多十五次一字节零值写，避免多个 worker 并发承担同一大页的首次写入。稀疏预留、多任务或多执行通道块、GPU scratch 及 Shared/Device 存储保留原路径。该有界提示只作用于已经准入且保证为零的缓冲区，不改变逻辑容量、生命周期、失败处理和资源计量。

执行器创建 Result 时使用 `max(ResultGrowthLimits{}.maximum_bytes, maximum_live_bytes)` 增长上限。默认配置仍为 1 GiB，显式提高 Context 容量可允许更大的单个 Result。保留的 backing 与发布元数据计入该上限，Root 的全部限制仍独立执行。

`ResourceAllocator` 在分配前准入元素容量与对齐头，直到物理释放之后才释放 lease。Metadata 容器计入 Host/Metadata；payload 容器的元素还计入 Payload。`BufferAllocator::limited()` 限制 backing capacity，`limited_requested()` 限制存活请求字节。Native 转换保留 allocation provenance，并按实际容量计费。绑定资源根的 quota/provenance state 与 reservation owner 使用托管分配；parent scope 链避免未计费的可增长 provenance 目录。Allocator 副本共享这些 owner；payload 释放后，其 metadata 保持到最后一个 allocator 或 buffer owner 退休。调用方 unmanaged allocator 不自动纳入执行资源根计费。

Host 与 native Shared 分配先预留容量，再创建 `CpuStorage`。Owner 构造与 payload 分配分别 commit，因此后续 payload 或 native bind/map 失败时，已观察的 metadata 峰值会保留，live 字节随后回滚。容量 sublimit wrapper 将两种增量都转发到原资源根。已经通过 `NativeAllocator` 分配的 native wrapper 对象不会在此路径重复 commit。

Block 分配分别 commit Admission、Slice、EndReservation 对象和每个成功分配的目录数组。Slice 退休时撤销 owner metadata 与已经释放的 backing，block 仍可继续保护 reservation。目录 move assignment 保持析构顺序。导出 Slice 在 block 退出后继续保留 Admission owner；最后一个导出释放时清除其剩余实际计数。外部引用别名共享一个 registration 对象，只计一次 metadata，借用 payload 不被当作新分配重复计量。

Result 目录、coverage 与 publication metadata 使用其选定资源根。新 tensor backing 使用 Payload；view publication 保留已有存储。`capture()` 固定 descriptor revision 与 coverage，后续发布不会扩大旧 capture。保留来源 Result 的 view 保持原有 backing lease，新 view 的资源根承担自身 metadata。直接发布外部存储时按适用规则调用 `reference(storage)`。

Result envelope 与 capture 对象在构造后、独占 owner 转换为共享 owner 前提交自身字节数。因此，共享控制块分配失败时，已计量对象会退休并保留其峰值。对已有 capture 再次 capture 会共享同一对象。Weak 引用不保留 envelope 或 capture 分配。每个 Result 的增长预算对象独立使用 `ResourceAllocator`，包含其托管共享分配块，因此增长 owner 可在 Result envelope 销毁后继续保留预算。

Host 与 Device tensor span 共享由 `ResourceAllocator` 分配的不可变 metadata 对象。其独立 metadata reservation 覆盖普通 shape、region、stride 和 origin vector。每次 vector 复制成功后，在开始下一次复制前提交实际 capacity；中途失败会退休已完成的副本。Metadata 对象已由其 allocator 计量，不再重复计算。Span 副本共享这些字节；新 subview 拥有独立 metadata 对象和数组。

`SchemaTemplate::managed_copy` 独立准入并计量每个 tensor 的 descriptor shape。Key 与 batch axes 使用自身托管 allocator；shape lease 不再重复预留 batch-axis 字节。普通 `ResultTensorSpec` 副本创建独立的普通 shape 存储，不保留来源 shape reservation；其 key 和 batch axes 遵循自身 allocator 的复制规则。Move 同时转移 shape 与对应 lease。Copy 和 move assignment 在退休原 lease 前销毁被替换的成员。执行资源根使用 `managed_copy` 准入受控 schema。

Read window 计量自身拥有的 Region 数组。窗口固定栈对象不计入实际堆字节，准入仍预留其簿记余量。Compose 先释放每个已消费的 Region 并退还其实际字节，再保留来源 lease；该 lease 继续保护已预留容量。嵌套 compose 保留先前来源 lease，不重复计量已释放的 Region 数组。向已有窗口 move assignment 时，被替换的数组先于其 lease 退休。

Affine view 映射在 scratch reservation 下计量临时 Region dimensions、descriptor shape、stride、origin 与来源坐标。Shape 在填入任一部分前，按完整 batch 与 cell rank 预留并提交容量，避免 vector 中途扩容。每个来源 piece 在处理下一个 piece 前释放自身 scratch。Affine 合并计量探测坐标和 layout 数组，随后借用 metadata 构造保留的 TensorSpan，不额外复制 origin。Read-window 合并入口在窗口资源根下准入 descriptor 与合并 scratch。Reshape 使用固定栈数组，最多保存 8 个 stride chunk，每个 chunk 最多 8 个 factor；这些数组不产生堆分配或 metadata 计费。

受控 sample descriptor 在构造 shape 前一次预留完整 batch-plus-cell rank，随后提交实际 capacity，并在 shape 析构后释放 lease。Sample count 使用 checked multiplication，不分配 shape。Footprint 的 `from_regions`、`from_region`、`all` 与 `none` 借用调用方 shape；`from_region` 同时借用单个 Region，避免隐式临时 Region/vector 副本。保留的 shape 副本与临时 whole Region 在后续分配前完成准入与计量。`close_samples` 计量 descriptor shape、临时 box 与每个 box 的 dimensions，部分失败同样正确释放。

Footprint 规范化在向不可变 owner 转移前，计量 canonical box 数组及各 box 的 dimensions。Owner 增加自身对象和 shape 字节，不重复计算转入的数组。Lease 别名在参数构造与失败展开期间保护这些数组。Visit 坐标在 visitor 回调期间保持计量。Tile-cover 构造计量临时 box 表及每个已完成的 dimension 数组，直到规范化返回。其 shape 拥有独立 lease，随 shape 转入返回的 Footprint，不复制或重复计量该 shape。分配失败与取消时，临时数组先于对应 reservation 释放。FragmentAtlasPlan 计量保留的 descriptor 与 geometry 容量；其 allocator 已计量共享 owner 分配块和 tile 表。自动 geometry 在填充空 vector 前先预留所需 rank，包含 vector 已有 capacity 的情况。两类不可变对象的副本共享原有分配。

FragmentAtlas 实体化在当前 metadata 资源根下准入并计量临时坐标数组。返回的 descriptor shape 与 tile geometry 拥有独立保留的 lease，每次复制成功后先提交计数，再开始下一次分配。Move 同时转移数组及其 lease；替换时先销毁旧数组，再释放其 reservation。普通 Atlas 副本创建调用者拥有的 geometry，并共享 packed TensorSpan 存储，不保留来源 geometry lease。实体化分配失败返回 `ResourceExhausted`，退休每项已完成分配。

Packed 与 bulk tensor publication 在准入的 scratch reservation 下计量临时 coordinates、chunk Region、window/layout 数组、descriptor shape、stride 和 origin。每次分配成功后，在下一次可能失败的分配前 commit；guard 比所有受保护数组更晚析构。最大 rank 8 的排序数组保留在栈上。Canonical window clipping、显式复制和标量读取计量其临时坐标与 Region，并返回 typed allocation failure。由 window 创建 affine TensorSpan 时使用该 window 的资源根。

Input snapshot 另外使用自身的总容量与工作集 reservation。Snapshot reservation seal 后释放未使用容量；已分配块持续计费到最后一个 owner 释放。这份配额与执行资源根的计量分开。

`ImmutableResource` 拥有不可变字节及字节长度/SHA-256 identity。Import 复制并哈希，`from_storage` 哈希已有存储。`ResourceSet` 精确选择声明的 identity 并保留 backing。Plan 及其 prepared operation owner 独立于输出 Result 保留这些资源。`ResourceSet::reference` 显式在另一个资源根下准入集合，不复制 payload。

## 上限与限制

容量乘积、对齐与取整在准入前使用检查算术。恰好填满剩余容量的请求可以成功。`live` 包含已经预留但尚未使用的容量；`peak` 是观测到的最大准入容量。分配与 work 计费发生溢出时失败，不发生回绕。

`WithinBudgetOrFail` 只保证已纳入计量的容量。编译器存储、调用方拥有的 binding、在托管分配器外制作的副本，以及 provider 私有分配，不会仅因 Plan 或 kernel 可以访问它们就获得执行资源根上限。静态结构上限、执行 work 上限与托管内存上限分别描述不同约束。
