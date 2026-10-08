# 分页四连通分量与面积索引

英文权威版本：[Paged-Components.md](../Paged-Components.md)。

## 1. 模块边界与职责

CPU 操作 `components4.labels`、`components4.area` 和 `components4.filter` 计算四连通划分、派生有序面积索引，并按面积筛选标签。Labels producer 在完整 raster 扫描期间持有磁盘支持的 union-find 表。Complete Result 一起发布 labels 与 component rows。Area 和 Filter Result 将输入 ObjectId 存作 association 事实；这些 ID 不会保活所引用的 payload。

## 2. 核心数据结构与内存布局

```cpp
enum class ComponentIdScheme : std::uint32_t { MinPixel = 1, CompactMinOrder = 2 };
struct ComponentsSpec final {
  std::uint64_t height = 1, width = 1, maximum_count = 1048576;
  ComponentIdScheme ids = ComponentIdScheme::MinPixel;
};
Result<SchemaTemplate> component_area_schema(const ComponentsSpec& spec);
Result<SchemaTemplate> component_filter_schema(const ComponentsSpec& spec);
Result<OperationDefinition> make_component_operation(
    ComponentOperation operation, const ComponentsSpec& spec);
```

当前工厂支持 `MinPixel`：HW 尺寸为正，校验 `H*W <= (INT64_MAX-4095)/32`，并要求 `maximum_count <= INT64_MAX`。Labels 接收一个 Result，其中有一个 unbatched、无 facet 的 UInt8 HW tensor，且不含 fields。输入验证依据 tensor type/shape，不要求固定 numeric Result schema ID。任意非零字节表示前景。背景标签为零。前景组件 ID 是最小行主序前景像素位置加一。同一输入快照中的 ID 确定，但后续输入编辑造成连通区分裂或合并时，ID 可以变化。

| Operation | 输入与输出 |
| --- | --- |
| `components4.labels` | UInt8 HW tensor Result -> CompleteBundle labels 与 `(id,area,min_position)` 行 |
| `components4.area` | 完整 Components -> CompleteBundle 有序 `(id,area)` 行及 RuntimeCount |
| `components4.filter` | Components 及其关联面积索引 -> CompleteBundle UInt8 HW mask |

Area index schema 保留 Components basis。Filter schema 保留相同 basis，并要求正 Int64 `minimum_area`；该参数进入 operation identity。每个像素仅当 `label!=0 && area[label]>=minimum_area` 时输出 1。`maximum_count` 限制最终组件数，不限制 N 个 labels 和 N 个临时 union record 的工作空间。N>0 时 K=0 仍合法；此时 labels/mask 全零，表无数据行。

## 3. 调度与状态机

```text
variant Result binding -> mask source --Need--> UInt8 HW tensor Result
                                                   |
                                                   v
                                     labels producer -> 完整 Components Result
                                       |                |
                                       v                |
                                 area producer          |
                                       |                |
                                  area index            |
                                       +-------+--------+
                                               v
                                            filter
                                               |
                                           二值 mask
```

Labels 读取有界 source strip，完成所有私有 union 写入和第二次输出扫描后，才发布 CompleteBundle。Area 等待完整且已验证的 Components 结果。Filter 同时要求精确的 Components Result 和其 area index；它先验证关联及行数，再把每条索引行与完整 Components 表比较，之后才处理像素。索引关联不匹配返回 `TypeMismatch`，原因是 `InvalidAssociation`，scope 为 `Association`，并记录 index ObjectId。非零 label 没有关联 area 时也会失败，不会把面积当成零。

三个操作都使用 CompleteBundle，并通过 typed Tensor、Field 和 Descriptor relation 表达 Conservative(All) support。Tensor relation 按逻辑 sample 定位；Field relation 按字段行定位，因此一个含三个 Int64 值的 component row 仍计作一行。Descriptor support 与数据行分开，K=0 时仍存在。结构校验会检查计数和 basis，但不证明导入标签图连通。只有 labels 操作自身的构造保证其输出为四连通结果。Area 将 component rows 复制到自己的 Result fields；Filter 通过有界 Field window 读取两个输入 Result。已加载的 field `CpuStorage` 会保留其 read plan 和 Result 实现，因此即使 context 与 Result wrapper 已释放，字段 backing 仍可读取，直到最后一个 window 释放。

Labels operation 声明 8192 字节 callback workspace；area 和 filter 各声明 4096 字节。当前窗口上限为 `min(user_page_bytes,1024)`；labels 至少需要 32 字节，area/filter 至少需要 24 字节。根预算负责临时 backing、window、work、I/O 和 stage。替换脏页前先写出旧页，再读取替代页。完整发布前资源耗尽会释放私有状态，不发布部分 labels/table。

## 4. 算法与数学

Labels producer 创建一个私有临时文件，并扩展至经检查的 `32*N` 字节。每像素占四个 64 位字：背景为 `[0,0,0,0]`；前景位置 `i` 为 `[i+1,0,i,1]`，依次表示 parent address、rank、最小位置和面积。Parent address 从 1 开始；最小位置从 0 开始。

Producer 按行主序访问像素，只处理左、上边。每条边通过有界分页 parent 读取查找根。根相同时跳过；否则按 rank 合并、累加不相交集合面积并保留较小位置。处理下一邻居前先更新两条缓存记录。当前前景像素处理首条边前仍是一个根，因为更早像素只访问更小索引。全部边完成后，第二次扫描查根，写入 `minimum+1` 标签，并只在最小位置追加 `(id,area,minimum)`。这样无需驻留排序表即可生成唯一有序行。

四连通网格的每条边只由 left/top 方向访问一次。Union 不会连接不同连通区，扫描又包括各连通区的全部边。根 minimum 与 area 分别由取最小值和不相交集合求和保持。仅按 rank 的 parent 链深度为 O(log N)；实现检查 record 地址，并把查根限制在 64 跳。算法不做路径压缩。

Label 生成和最终查根复杂度为 `O(N log N)`；面积索引复制/校验为 `O(K)`；过滤为 `O(N log K)`。表示校验还会对 K 条 table row 各扫描 N 个 label，并对非零 label 二分查表，工作量为 `O(NK + N log K)`。分页字段交替访问可能导致窗口重读，因此实际 I/O 不能只按最终 payload 计算。

## 5. 限制与非目标

- 这些工厂只支持 `ComponentIdScheme::MinPixel`；`CompactMinOrder` 是表示枚举，不代表工厂能力。
- Validator 检查 labels/table 精确成员关系和 basis，不验证任意导入 labels 的连通性。
- `maximum_count` 限制最终 K，不限制临时 N 条记录。N>0 时 K=0 合法。超过数量限制会在发布前作为无效 domain 失败。
- 全部 N 条 label、N 条临时记录、I/O、work 和输出容量仍需要根预算准入。Page、disk 或 stage 耗尽返回资源错误。
- Managed-capacity 计账不构成进程 RSS 上界。

公共入口与可运行用法见 [components workflow](../../../examples/components_workflow/README.md)。
