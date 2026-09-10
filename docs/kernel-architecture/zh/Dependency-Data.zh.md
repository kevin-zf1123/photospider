# 精确依赖数据

G4 数据 API 提供精确集合、稀疏不可变片段与逐观察解析证书。执行集成是独立的实现层；
这些类型本身不授权算子合批错误、消费 RequestRecord 或绕过 producer/snapshot 来源。

Footprint 表示非零 rank 1–8 逻辑域中的精确集合，以规范不交矩形表示 Empty、All 与
非连续覆盖。递归轴扫描仅在后续轴集合相同时合并相邻区间，因此插入顺序、重复矩形
与不同分块不影响集合相等。即使完整域元素乘积溢出，All 也保持压缩。并、交、差要求
相同域并保留空洞。tile_cover(geometry) 返回 ceil-divide tile 域中实际相交的 tile 坐标，
不把 tile 内所有样本标为有效。visit 在显式样本上限内按 row-major 顺序逐个访问。

FootprintLimits 对每次集合操作限制候选工作与矩形条目，包括重复构造工作。超限返回
ResourceExhausted，取消返回 Cancelled，两者均不等于 Empty 或 bbox 近似。复合调用方
还需限制跨调用的总工作和元数据；单次集合额度不是完整执行预算。

ValueFragments 保存完整 descriptor/facets、授权 Footprint 与有 owner 的矩形 Value。
构造裁剪输入覆盖，拒绝缺失样本和不一致重叠；同 owner 的等价 origin-relative 映射去重。
每个 typed image fragment 及授权矩形都须包含完整 C，不能以两个部分通道 Value 拼接
规避。Generic 数组允许任意样本子集。read 通过 Value 的 checked signed-stride 地址
复制实际 dtype 宽度；空洞、未授权地址、错误宽度均失败，不同步取数或补零。
restrict 同时限制 owner 与覆盖；collect 在通过完整矩形检查后使用 BufferAllocator
分配。retained capacity 统计实际唯一 storage owner，与有效区域大小分开。

DependencyCertificate 保存身份、精确观察覆盖、声明输入域及每项观察的 AtomCertificate。
Generic 观察是逻辑 sample；图像观察域为 HW，每项代表完整像素。Row 保存输入端口、
各项依赖 role、精确样本 Footprint 和非空间 tagged atom。多角色输入规范展开，发布前
限制展开后的元数据。显式空 row 表示已知空，缺失 row 表示未知，不能发布为完整解析。
身份绑定调用方的访问/数值/错误合同及快照；数据类型不能证明任意 callback 遵守读取声明。

restrict(P) 拒绝覆盖域以外的 P。backward(P) 仅按端口/role 对相应 row 求取数并集，
该投影不是证书。transpose(dirty) 精确返回覆盖域内与相同端口、相交 role 的样本/tag
支持相交的观察。merge 要求身份/域一致，重叠观察的规范 row 相同。请求级失败证据及
终端 RequestRecord 不属于原子成功证书。

内部 DirtyDeltaQueue 在一个证书 generation 内为每条记录保存 accumulated 和 propagated。
同一 mutex 下取 accumulated−propagated、记录已传播集合并清 queued；后到的新原子
可以再次入队。依赖结构由 demand coordinator 持有，与像素缓存 owner 分离；队列自身
不拥有像素或 worker。任何下游入队失败都必须令该 generation 失败。

Focused 检查为 test_footprint、test_value_fragments、test_dependency、test_dependency_dirty
及 test_input_snapshot，使用独立有限集合/可达性 oracle，覆盖未知 row、identity/swap、
role/tag 隔离、迟到 dirty、dtype/stride/owner 上限和快照 COW。通用快照身份与所有权见
[缓存模型](Cache-Model.zh.md)。
