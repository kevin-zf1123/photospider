# 分页四连通分量与面积索引

英文权威文档：[Paged-Components.md](../Paged-Components.md)。

Package 0.10 的安装 C++ API 提供 `make_component_operation`、
`component_area_schema` 和 `component_filter_schema`。C operation ABI 保持 9，
既有 Float32 `mask.components` 的 compact-label 行为不变。

## 公开 profile

这些工厂要求 ComponentsSpec 的 HW 为正、`H*W <= (INT64_MAX-4095)/32`、
`maximum_count <= INT64_MAX`，且 ID 模式为 MinPixel。输入为无 facet 的
UInt8 HW mask，任意非零字节都是前景。命名基底 `components_min_pixel_v1`
使用背景零和 `1+最小行主序像素位置` 作为组件 ID。ID 在快照内确定；编辑造成
分裂或合并时不保证保持。

| 阶段 | 输入与输出 |
| --- | --- |
| `components4.labels` | UInt8 HW → 完整 Components：N 个 Int64 labels、K 个 Int64[id,area,min] 行 |
| `components4.area` | 完整 Components → RuntimeCount K 的完整有序 Int64[id,area] 索引 |
| `components4.filter` | Components 及其面积索引 → 完整 N 行 UInt8 0/1 mask |

索引 schema 为 `photospider.component_area_index`，字段 `rows`，metadata
`component_area_basis_v1` 保留 Components specification。过滤 schema 为
`photospider.component_filter`，字段 `mask`，metadata 为
`component_filter_basis_v1`。二者保留 HW domain 和完整基底。过滤必需参数
`minimum_area` 为正 Int64，进入算子语义 key；支持完整正 Int64 范围，不把
阈值转成浮点数。输出严格等于 `label!=0 && associated_area[label]>=minimum_area`。

`maximum_count` 仅限制最终 K。K=0、maximum_count=0、N>0 合法：labels 有
N 个零，Components 与 area 表均零行，filter 产生 N 个零。该上限不能替代
固定 N 个 labels 或私有 N 个 union 记录的容量准入。组件数超限在发布前返回
OperationFailed/InvalidDomain，origin 为 Domain，scope 为 Group。页、磁盘和
工作预算耗尽仍报告 ResourceExhausted。

## Rank-union recipe 与基底证明

1. 创建私有临时文件，一次 Extend 已检查的 32*N 字节。有限 callback 读取
   source strip，写入连续记录：背景 `[0,0,0,0]`；前景像素 i 写入
   `[i+1,0,i,1]`，依次为 parent、rank、minimum、area。parent 是一基地址，
   minimum 是零基位置。
2. 按行主序遍历前景，x>0 时合并左邻，y>0 时合并上邻。页边界不改变邻接。
   当前像素首次处理边之前仍是自身根，因为先前像素只访问更小索引。在两次
   合并之间保留当前组件根；其他根获胜时同时更新地址和完整 record。
3. 通过有界分页 parent 读取查找邻根。同根不重复合并；不同根按 rank 连接，
   对不交集合面积求和、对位置取最小值。两个缓存记录都更新后才处理下一个邻居。
   非根 area/minimum 可以过时，不能作为根事实使用。
4. 全部边完成后重新扫描像素并查根，输出 label=`minimum+1`；仅在扫描像素
   等于 minimum 时追加 `[id,area,minimum]`，自然形成唯一有序表，无需驻留
   排序表。全部写入结束后 labels 和表一起发布。

left/top 定向使每条网格边恰好访问一次。union 不会合并真正不同的连通分量；
每条路径的全部边均包含，因此结果恰好是四连通划分。根 minimum/area 由最小
值和不交集合求和保持。根地址可以不同于发布 ID。无路径压缩的 rank parent
链为 O(log N)，实现检查地址及 64-hop 上限，不声称逆 Ackermann 复杂度。

2×127 梳形图中 N=254、前景=191、私有 UF payload=8128 字节。本两遍初始化
方案在 union 开始时有 191 个根；设计中的 65 个临时行组件是另一种计数调度
示意，不是本实现的实际初始状态。最终 K=1。

## 索引验证、支持与所有权

Area 等待完整、已验证的 Components，以有限页复制 ID/area 对，association
精确指向该 Labels ObjectId。Filter 首先检查此关联及相等 K；相同形状、K
甚至相同数值也不能替代对象关联。过滤前逐行比较完整索引与 Components 表，
从而验证外部索引的顺序、正面积和完整性。失败使用 InvalidAssociation、
Association scope 和索引 ObjectId。查找采用分页 lower_bound 和一个保留
cache window。背景跳过查找；非零 label 缺失属性时失败，不默认为 area=0。

三个阶段均为 CompleteBundle，关系保持 Conservative(All)，K=0 也保留独立
descriptor 支持。导入 Components 的 count/basis 验证与连通性证明不同；新
labels recipe 和独立 BFS 为本实现生成的结果提供连通性证据。输入关联及
读取窗口在 ExecutionContext 销毁后保留上游和强制存储，直到最后 owner 释放。

## 实际资源边界

UF 占 32*N 逻辑磁盘字节，按 4096 字节编码 extent 准入。Create、Extend 和
依赖写入是不同 coordinator 阶段。source、union、输出窗口最多为
min(user page,1024) 字节。Labels 至少需要 32 字节窗口，area/filter 至少
需要 24 字节。两个输出 slab 和可能更小的最后表复制具有有限重叠容量。
Labels 在 edge、parent find、两条 union 更新和最终 emit 之间保留四个可写
LRU 页。替换脏页时先写出再读取新页；命中缓存时在当前 poll 内继续。两个
union 记录都驻留后才应用缓存更新。emit 使用同一缓存，能读取最新根事实。
最后 append 完成后可将剩余私有脏页和 UF 一起丢弃，因为后续不再读取私有树；
关联验证只消费发布字段。

Labels 声明 8192 字节 callback workspace，包含最多 4096 字节缓存树 payload
以及有界输出 slab/尾页复制。area/filter 仍声明 4096 字节。读取窗口 owner、
metadata 及跨阶段字段另按实际所有权计入根预算。

Union 和最终查根为 O(N log N)，索引复制/比较 O(K)，过滤 O(N log K)。现有
Components validator 另需 O(NK+N log K) 工作，在 labels/table 间切换时
可能反复加载单页。ResultBuilder 小批追加也会重写对齐 padding。因此验证
I/O 和 padding 写入不能仅按最终逻辑 payload 配置预算；实际工作和 I/O 均
累计计账，超限失败并释放私有部分结果。公开示例使用有限的一百万阶段上限
及显式根预算。

[components_workflow](../../../examples/components_workflow/README.md) 提供
公开执行、安装命令、精确 BFS 参考、动态/空集合、分页面积索引及超过 Host
容量的数据。测量来自产品 managed-capacity 计账，不是进程 RSS 硬上界。
