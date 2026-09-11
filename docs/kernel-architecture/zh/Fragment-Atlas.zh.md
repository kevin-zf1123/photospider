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

当前已实现 transport 和 shader lookup，staged GPU 调度及有界 GPU discovery 仍在
G4 实施范围。依赖 Run 暂时仍拒绝 native stage。Float64/Int64 原始 bits 传输不代表
已支持相应 GPU 浮点算术。

公开安装 consumer 使用 `test_fragment_atlas.cpp`，验证四 dtype、wire words、bit63
及孔洞、输入 bits 变化、负 stride、全域超过 uint64 的 rank8、allocation/work 精确边界、
源 owner 退休和分配时取消。非安装 native test 在真实 Metal 上执行同一 SDK helper，
独立坐标字典检查结果。命令见英文文档。

Native 测试有七次 dispatch，覆盖 rank1/3/8、width1/4/8、65 个离散 fragment 使用四个
binding、超过 16 KiB 的目录实际 capacity、少一字节分配失败、全局越界与内部孔洞。
返回 77 为无原生 Metal 的 skip，不是原生成功；transport 成功也不代表 staged GPU/
discovery workflow 已完成。
