# 稀疏 fragment atlas 传输

[英文实现说明](../Fragment-Atlas.md) 为权威来源。`FragmentAtlasPlan` 从精确
`ValueFragments` coverage 准备传输元数据，再通过调用方 allocator 物化。支持 rank
1..8 和 UInt8/Int64/Float32/Float64，保留样本 bits。Plan 只持有域、coverage 和 tile
mask，不保留源 Value/storage owner。物化读取当前匹配输入，支持负 stride 和多个
物理 fragment，不填充 bounding-box 间隙。

每个 tile 至多 64 个逻辑样本。可指定正值、rank 对齐且乘积不超过 64 的 tile extents；
默认从最右轴开始确定性填充。仅占用 tile 进入目录。Lookup 使用逐轴全局 tile 坐标，
不要求全局 shape 乘积可表示为 uint64。图像 C 坐标保留在原域内，此传输不改变
image-v2 closure 或供给验证规则。

目录 slot 数为 2 的幂，至少为 2 且至少为占用 tile 数的两倍，使用开放寻址和线性探测。
每个 slot 为 80 字节 little-endian：0..63 是八个 uint64 tile 坐标（未使用轴为零），
64..71 为 uint64 有效样本 mask（零表示空 slot），72..79 为 uint64 payload 字节偏移。
Hash 对 rank 个坐标逐字节（低位先）执行 FNV-1a，offset basis 为
14695981039346656037，prime 为 1099511628211。Tile 内 row-major 坐标选择 mask bit；
地址为 tile 偏移加此前有效 bit 数乘 dtype width。Payload 按 tile key 和有效 bit 顺序
紧凑存放，孔洞不占样本。空端口使用一个惰性 payload 字节和两个空 slot，均不授权样本。

Prepare 在物化前报告两份逻辑 allocation 大小。GPU 宿主应分别按设备实际 capacity
取整再 admission。`maximum_boxes` 限制占用 tile，`maximum_work` 限制元数据扫描、
样本发现、hash/probe 和 packing；cardinality/equality 扫描前检查取消并预扣 metadata。
成功 plan 提供 `preparation_work()` 和 `materialization_work()` 供外层 Run 计量。
任一 allocation 后取消（包括 Empty）都会释放新 owner，不发布 atlas。

公开 `FragmentAtlas::address` 与 C 兼容 SDK 宏 `PS_FRAGMENT_ATLAS_MSL_V9` 的查找
一致，后者定义 Metal `ps_atlas_address`。宿主提供真实 binding span、slot 数、shape、
tile extents 和 payload 字节数。任意 fragment 数仅需 payload 与目录两个 binding。
边界映射必须先作用于全局坐标。缺失 slot/mask bit 是明确缺页，不是零值或逐 fragment
clamp。

C++ staged GPU Run 已接入下述 transport。C staged GPU 桥接已接入；[有界 GPU discovery](GPU-Discovery.zh.md) 已接入；
同步 GPU producer 使用下述精确矩形传输。Float64/Int64 原始 bits 传输不代表已支持
相应 GPU 浮点算术。

公开安装 consumer 使用 `test_fragment_atlas.cpp`，验证四 dtype、wire words、bit63
及孔洞、输入 bits 变化、负 stride、全域超过 uint64 的 rank8、allocation/work 精确边界、
源 owner 退休和分配时取消。非安装 native test 在真实 Metal 上执行同一 SDK helper，
独立坐标字典检查结果。命令见英文文档。

Native 测试有七次 dispatch，覆盖 rank1/3/8、width1/4/8、65 个离散 fragment 使用四个
binding、超过 16 KiB 的目录实际 capacity、少一字节分配失败、全局越界与内部孔洞。
返回 77 为无原生 Metal 的 skip，不是原生成功；transport 成功也不代表有界 GPU discovery 已完成。


## Staged native execution

GPU `DependencySession::poll` 要求宿主提供完整 `DependencyGpuServices`。Run 复用
现有 GPU worker、waiting admission、native device 和同步 `Invocation`；start/source
仍使用 CPU worker。状态缓存命中及 control/constant-only 路径可以没有新 dispatch。
backend 表示实现及数值契约；dispatch/submission/device-time 和 poll timing 记录实际工作。

`phase.atlas(port)` 按需准备当前 stage 已验证的精确端口，准备与打包工作在分配前扣除，
同一 poll 重复调用复用相同 atlas。未供给端口及 CPU 调用失败。native buffer/execute
服务错误和异常 sticky，忽略错误也不能发布成功。宿主持有 native view 直到同步 callback
完成 drain。普通、stream 和 frozen Run 合并 caller 与 sets cancellation；取消优先于
此前 native service 错误。

每个 atlas 的 payload/目录按分别取整后的真实 native capacity 执行独立非阻塞 reservation。
stage 输出按各矩形分别取整，workspace 声明包含每次 native 分配的真实 capacity。
seal 只释放未用 reservation；atlas、state、scratch/output 活跃 owner 仍计费。无法与
已持有 owner 一同放入预算的 stage 有限失败，atlas 不扩大读权限、不授权合批。

公开 `examples/g4_gpu_workflow/main.cpp` 先在 host 读取每个观察的 Int64 control，
声明 65 个离散 Float32 样本，再用三个 binding 执行真实 Metal 求和。独立预期为
2145、4290、2145；前两个观察 dispatch，第三个复用相同 incoming/control 和当前 data
的 pure block，并保留自己的 control `{2}` 证据。shader 检查缺失样本后才允许数值发布。
另验 33079/33080 字节 stage reservation 失败/成功临界值及 owner 复用，以及普通 stream
在 native service 错误后的辅助取消优先级。运行命令见英文文档。

`test_dependency_gpu` 使用明确非 native 的 mock，验证服务缺失、CPU 误用、异常、忽略
错误、先计工作后分配和 atlas 复用/退休；不构成 native 证据。static/shared 安装 consumer
运行此测试并编译相同公开 workflow。没有 native Metal 时 workflow 验证 CPU 后返回 77。


## C staged GPU 桥接

`ps_dependency_services_v9` 与 `ps_dependency_block_services_v9` 均提供 `atlas`、
`gpu_buffer`、`gpu_execute`，返回布尔 1 成功/0 失败，与 `ps_gpu_service_v9` 结果码不同。
`ps_dependency_atlas_v9` 给出全域 shape/tile、slot 数、有效样本字节、真实 binding span
和不可变 payload/目录 token，不增加源读取权限。同 poll 同端口复用 token；CPU 调用拒绝。

适配器把 native view token 映射成会话内单调 C handle，只接受当前 poll 的映射，旧 poll、
伪造或其他类型 handle 无法误用被重新编号的 native token。发布输出撤销既有 token 的
写权限。每批最多 32 commands、每 command 31 bindings、每 poll 1024 views，复制前计
command/binding metadata 工作；底层仍核验 source、constants、access/grid 边界。
错误 sticky，包括 pure compute 忽略错误。atlas/view owner 保持到同步 drain 完成。

公开 C11 `c_plugin.c` 与 `c_main.cpp` 执行两个观察：host control 决定 65 个离散
Float32 样本，独立预期均为 2145。实际 1 dispatch + 1 block hit；第二观察保留 control
`{1}`，排除旧 `{0}`。native 输出/状态缓存合计 16 字节。负例覆盖旧/伪造 handle、
错误 atlas record、view/command/binding 参数、不变输入提权、发布后写和真实 shader
缺失样本。缺失样本在数值输出发布前失败。每个失败案例后，在相同的紧预算 context 内禁用结果
缓存并执行真实重算，排除旧缓存遮蔽失败路径 owner 未退休的情况。

英文文档提供命令。standalone CMake 与 static/shared 安装 consumer 均编译该 C11
模块及公开 loader；loader 可传模块路径。无 native 时先验证 CPU，再返回 77。
此桥接通过独立 callback 提供[有界 discovery request table](GPU-Discovery.zh.md)。


## 同步 producer 与 CPU 回退

依赖 Run 通过 context 原有 native worker 调用同步 GPU producer。host 按声明的矩形
输入需求收集到紧密 native storage，分别计入实际取整容量，并在 callback 生命周期内
提供 `ps_gpu_service_v9`。GPU callback 必须提交真实 native work。Invocation view
在 owner 退休或 CPU 重试前完成 drain；Whole 保持全域验证与证据。

仅 `BackendUnavailable`、支持 CPU 且 `allows_cpu_fallback` 时重试。staged 失败后
退休 continuation，在 CPU worker 从原始 Q 重新 start。取消优先；每个 attempt 使用
per-session 限额，所有 attempt 计入同一 Run 工作上限。诊断记录实际 backend 和拒绝
attempt。回退来源随子结果、shared Flight、Whole record 传播；禁止保留 dependency
结果缓存，后续阶段也不使用或保留 checkpoint/pure block。waiter 取消继续独立。

staged GPU attempt 前，Run 保存有界记录覆盖域。CPU 重启恢复该覆盖域（包含此前
合并的 rows），重建本地索引，弃用的祖先不再占用重试的记录额度。保存和恢复计入
工作量，GPU discovery 已消耗的工作不退还。共享不可变记录、Flight 与 cache epoch
保持不变。Whole 像素及完整结构 DAG 保持每 Run 至多一次的生命周期，仅在重启路径
实际请求这些像素时重新导入证据。

公开 `sync_main.cpp` 以 `x+1` 独立 oracle 检查离散 `{0,2}` 得到 1、3，验证真实 Metal、
无设备 CPU 回退、Whole 和 staged 中途回退后再消费的 GPU 后代、attempt 诊断、无旧
缓存命中的 native 重试及 Whole 全域证据。两个 20000 字节 payload 的 native 分配各取整为 32768 字节，总预算
65535 失败、65536 成功；在同一预算内释放前次输出后也可完成回退与再次 native 重算。无 Metal 时先检查无设备回退再返回 77。
`test_execution_demand` 另检查共享回退中的 owner waiter 取消及零缓存保留。命令见英文。
新增原生案例验证 Elementwise 祖先后 16-entry、Whole 祖先后 12-entry 预算内的
CPU 常数回退，以及此前输出 rows 恢复、普通与 frozen 执行的 Whole 复用和完整证据。
可移植记录测试验证本地回滚后，独立共享的祖先/子节点 DAG 仍可重新导入。
