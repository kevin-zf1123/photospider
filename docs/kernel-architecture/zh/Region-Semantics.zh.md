# 逻辑 Region 与 Result 图像 samples

英文权威文档：[Region-Semantics.md](../Region-Semantics.md)。

## 模块边界与所有权

`Region` 描述 descriptor domain 中的逻辑 samples。`Footprint` 保存这些 samples 的精确集合，包括不相邻的 boxes 和 holes。Storage origin、byte offsets、strides、padding、planar pages 和 tile geometry 描述物理访问方式，不会重新定义逻辑坐标或有效 coverage。

对图像而言，descriptor domain 外加 Result frame 和 layer axes。`ResultRef` 拥有图像语义、descriptor、已发布 coverage、dependency relation 和生命周期。`PlanarImage` 为 Result image slot 提供 backing，并读取获准的 samples。Planner 确定请求范围，operation 声明输入 support，coordinator 只准入请求所需的读取和发布。

## 核心数据结构与内存布局

```cpp
struct ResultImageSpec final {
  ResourceString key;
  std::uint64_t frames = 1, layers = 1;
  ValueDescriptor descriptor;
  PlanarImageLayout layout;
  std::vector<ValueFacet> facets;
  std::vector<std::uint64_t> sample_shape() const;
  Result<Footprint> close_samples(const Footprint& samples,
                                  const FootprintLimits& limits = {}) const;
};
```

片段省略了 fields 和 ownership helpers。逻辑图像形状为 `{N,L,H,W}` 或 `{N,L,H,W,C}`：`N` 选择 frame，`L` 选择 layer，`H/W` 选择空间 sample，可选的 `C` 选择 channel。当前实现要求 `N`、`L` 为正数且 `N*L <= 4096`，每个 frame/layer 的 descriptor rank 为 2 或 3。四通道像素可以放在分离的 planes 中；逻辑 tuple 完整不代表物理字节交错。

请求 semantic image sample 时，会保留 legacy image tuple 闭包，覆盖完整 channel axis；如果 alpha 是 channel，也包含 alpha。`ColorArray` 请求则根据经过验证的 facet 闭合到完整 tuple。只有 TDM 的 facets 和 structural layout groups 不会扩展 sample set 以包含 peer channels 或 alpha。没有上述 tuple facets 的 typed image slot 按 descriptor sample 坐标访问。Padding、plane gaps 和尚未发布的 samples 仍不可读。Storage views 保留 backing owner；Result coverage 及 captured descriptor 则授权 callback 可读范围。

普通 Value descriptors 和结构化 Result image descriptors 遵循不同契约。Value 的 extent 非零；在保留非零 descriptor 时，其请求 Footprint 可以为空。Result 的动态 structured field 可以有零行，image slot 的请求 footprint 也可以为空。两种空状态都不能伪造 samples 或省略 descriptor/count obligations。

## Demand、执行与状态

编译器解析每个具名 output 的请求 `Q`。对 image output，Result continuation 收到 captured output footprint 和 image slot。Callback 可分阶段请求 input needs：先取 Control samples，再根据 control 值选择 Data image samples。每个 Need 都依据 Result schema、slot、逻辑 sample domain 和 input 已发布 coverage 校验。Callback 不能同步取回空洞或当前 capability 以外的 sample。

`ResultRelation` 使用展平的逻辑 sample 坐标记录 output 到 input 的 support，并分别标识 input port、target、slot 和 role。Data、Control、Validation 和 Descriptor support 保持区分。Descriptor support 覆盖 count、basis 或语义 descriptor facts；静态 schema 变化要求重新编译。Frame 和 layer 坐标参与展平后的 image identity，因此 dirty edits 可定位具体 frame/layer samples。

Demand handle 替换 immutable bindings 后，coordinator 会将新 bindings 与 captured evidence 实际消费过的 source samples 比较。它根据旧 relation 计算可能变脏的 output coverage，再原子提交新 binding generation；新请求随后依据新 control 值发现并记录 support。因此，已消费的 Control 值变化可以使旧分支失效，并引入新选中的 Data samples。未消费且无关的 control 或 tile 保持 clean。Evidence object 本身不可变：先前的 dependency query 仍描述原 generation。

Tile 是调度单位，不是逻辑 dependency 证明。Tile projection 可将 samples 合并为 I/O 请求，但 relation support 仍基于 samples。Whole operations 保持 Whole dependencies。Regional 或 dependency operations 报告实际 support；tile grid 不会取代 relation。Tensor semantic metadata（例如 channel roles 和 ColorArray tuples）按声明的契约参与校验及 sample closure；物理 `PlanarImageLayout::groups` 不会暗中新增 dependency edges。

只有 captured relation 完整时，`Exact` support 才能证明 clean。`Conservative` support 可能产生更宽的 dirty set。`Unknown` 保持 unresolved，不能转换为空的 clean answer。Dirty 表示可能受影响，不表示数值不同。Relation traversal 和 Footprint transformations 有界，并计入 execution root；超出 work 或 metadata 限额时返回错误。

## Tile 投影的算术

对正数 tile extent `T` 和逻辑 extent `E`，tile 数量为：

$$
C = \left\lceil \frac{E}{T} \right\rceil,
\qquad
C = E / T + (E \bmod T \ne 0).
$$

该整数表达式避免计算可能溢出的 `E + T - 1`。Tile projection 仅为物理调度分组精确请求的 samples，不扩展其 dependency support 或 coverage。

## 限制与错误处理

越界 regions、无效 frame/layer 乘积、未授权读取、不完整发布和重叠 image writes 均会校验失败。Semantic image 和 ColorArray requests 会在访问前扩展到完整 channel tuples；无效 tuple description 或超出 slot domain 的 closure 会校验失败。Resource exhaustion、取消、stale binding replacement、sticky callback failure 和 protocol errors 保留各自状态，不会变成 Empty coverage。Coordinator 复用借用存储前，所有已准入 callbacks 都会退出。

空 output demand 不执行 image sample 工作，但 descriptor 和 control obligations 仍需明确记录。成功的 image publication 必须覆盖 captured output demand，并提供 relation evidence。此前已发布的 Result prefixes 保持不可变；后续 producer failure 不会扩大旧 descriptor 的授权。

Focused Result validation 已通过 N/L image samples、分阶段 Control 到 Data support、binding replacement、dirty transpose 和 semantic-alias diamond rebind。Installed Result contracts 与 public-workflow consumers 均通过。C11 fixture 验证了 runtime Field rows，包括零行与非零行之间的替换。`test_result_image_contracts` 覆盖跨 frame support 和各 slot 独立 dirty。
