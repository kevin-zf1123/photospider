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

公开 `FragmentAtlas::address` 与 C 兼容 SDK 宏 `PS_FRAGMENT_ATLAS_MSL_V8` 的查找
一致，后者定义 Metal `ps_atlas_address`。宿主提供真实 binding span、slot 数、shape、
tile extents 和 payload 字节数。任意 fragment 数仅需 payload 与目录两个 binding。
边界映射必须先作用于全局坐标。缺失 slot/mask bit 是明确缺页，不是零值或逐 fragment
clamp。

C++ staged GPU Run 已接入下述 transport。有界 GPU discovery、staged C GPU 桥接和
依赖模板中的旧同步 GPU producer 仍待实现。Float64/Int64 原始 bits 传输不代表已支持
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
