# 结构化表示契约

英文权威版本：[Structured-Representations.md](../Structured-Representations.md)。

## 1. 模块边界与职责

`representation.hpp` 定义八种闭集结构化 Result schema：Spectrum、Bands、PathSet、Points、Components、YCbCr420、Brush 和 Iterative。类型化 `*_schema` 函数校验静态身份；对应的 `*_spec` 函数解码该身份。Schema facet 参与 Result identity，且不依赖物理分页几何。

Coordinator 负责校验和发布。已识别的结构化 Result 使用 CompleteBundle：它通过有界窗口校验已关联并 seal 的字段，之后才通知 observer、缓存 Result 或交付 consumer。畸形对象不会作为空成功出现。动态行数不改变 schema，也不会产生虚构的零 extent Value。

## 2. 核心数据结构与内存布局

```cpp
Result<SchemaTemplate> spectrum_schema(const SpectrumSpec&);
Result<SchemaTemplate> bands_schema(const BandsSpec&);
Result<SchemaTemplate> path_set_schema(const PathSetSpec&);
Result<SchemaTemplate> point_set_schema(const PointSetSpec&);
Result<SchemaTemplate> components_schema(const ComponentsSpec&);
Result<SchemaTemplate> ycbcr420_schema(const YCbCr420Spec&);
Result<SchemaTemplate> brush_schema(const BrushSpec&);
Result<SchemaTemplate> iterative_schema(const IterativeSpec&);
Status validate_representation(const ResultRef&, const ResourceBudget&,
                               std::uint64_t maximum_window,
                               const CancellationToken& = {},
                               const std::function<Status(std::uint64_t)>& = {});
```

每个 schema factory 都在数据 I/O 前检查枚举、尺寸、数量和算术。Schema 以不可变 metadata 保存所属族的契约。Result 字段与 metadata 分开，因此逻辑形状或坐标声明不代表物理连续。

YCbCr420 校验 Y 范围 `[0,1]`，Cb/Cr 范围 `[-0.5,0.5]`。默认原色、transfer 和 reference 字符串分别为 `srgb-d65`、`bt709`、`display`。Chroma 的 extent 按半尺寸向上取整，名义 origin 为 `(0.5,0.5)`，step 为 `(2,2)`；边缘裁剪后的 source support 与名义坐标分开。Schema 不执行转换或文件 I/O。

| Family | 数据与身份 |
| --- | --- |
| Spectrum | Float64 实部/虚部对；原始 shape、变换轴/顺序、shift、packing、sign、normalization、采样 origin/step/unit、实数策略与容差 |
| Bands | 拼接的系数字段和 keyed members；每个 member 保存 shape、parent shape、origin、step、phase 与显式零状态 |
| PathSet | 唯一几何 authority；verbs 或 primitive 引用、几何 payload、子路径 offsets 及可选属性 |
| Points | IDs、有限 positions/attributes 和排序后的 `id_to_row` 对 |
| Components | Dense labels 与 `(id,area,min_position)` 表 |
| YCbCr420 | 固定 BT.709/full-range schema identity 及关联的 Y/Cb/Cr 字段 |
| Brush | state identity、carry state、pending events/positions 和已定稿的 event/dab 前缀 |
| Iterative | generation/iteration/stop reason、estimate、diagonal、右侧、测得 residual |

Spectrum 的 packed axis 保存该轴 `0..floor(N/2)`；原始尺寸保留奇偶长度身份。Packed spectrum 要求 shift 为零。默认约定使用负号且不缩放的 forward transform，inverse 除以所有变换轴长度的乘积；显式 normalization mode 会形成不同 schema identity。Bands v1 定义 duplicate-last Haar：`low=(a+b)/2`、`high=(a-b)/2`；合成时相加/相减，并裁剪到记录的 parent shape。必需 Mallat 成员是最终 `(L,0)` 和 level 1 到 L 的所有非零 axis mask。Missing、duplicate 和 ExplicitZero 含义不同。ExplicitZero member 有逻辑 shape 但没有系数区间；`band_range` 对缺失 key 返回 `NotFound`。

Path 坐标恰含两个有限 binary64 值。Core verbs 为 M/L/Q/C/Z，对应控制点数 1/1/2/3/0。非空子路径以 M 开始；Z 只能结尾，且与 `closed` 字段一致。空 path 的几何为空、offsets 为 `[0]`；单个 M 表示零长度开放子路径。Primitive authority 使用 `PrimitiveRef(tag,record_index,payload_member,association)`，其中 association 在 Int64 宽度字段中保留 unsigned ObjectId 位。属性 domain 为 Path、Subpath、Segment、Control 和归一化 ArcLength；插值为 Constant 或 Linear。属性值跨度完整且不重叠；归一化弧长参数严格覆盖 `[0,1]`。

| Field | 存储记录 |
| --- | --- |
| 0 | Path verbs |
| 1 | 每个 verb 的 control offsets |
| 2 | Float64 控制坐标对 |
| 3 | 指向 verbs 的 subpath offsets |
| 4 | 每个子路径的 closed 标记 |
| 5 | Primitive tag、record index、payload field 和所属 ObjectId association |
| 6 | 四组 Bezier 控制点；Line/Quadratic 未使用部分填零 |
| 7 | Arc center、轴 u/v、start angle 和 sweep 或 full-turn direction |
| 8 | Hermite P0/P1/d0/d1；导数使用归一化 t |
| 9 | Spline degree、首个 control/count、首个 knot 和首个 weight（非有理时为 `-1`） |
| 10-11 | Scalar Float64 knots 和 weights |
| 12 | 属性 owner、interpolation、component 宽度/数量、value offset、parameter offset |
| 13-14 | 属性值和归一化弧长位置 |

Primitive authority 要求 core-verb 数组为空，control offsets 为 `[0]`。Primitive Segment 属性索引 PrimitiveRefs；Core Segment 属性索引 verb 记录。单独开放的 Arc 或端点相等性未确定的 unclamped spline 仍可表示；需要未证明端点吸附的混合子路径会失败。

即使记录物理重排，Points 也要求 ID 到 row 的完整双射。InputPosition ID 留在固定输入 basis 内；Ordinal ID 为 `0..count-1`。`maximum_count` 表达语义数量上限，不能替代物理资源准入。

## 3. 调度与状态机

```text
schema factory -> immutable family identity
                         |
producer -> sealed 关联字段 -> coordinator 有界校验
                                    |             |
                                全部通过         失败
                                    |             |
                             observer/cache/consumer  不发布
```

Coordinator 在发布前校验已识别的 schema。校验通过有界窗口读取数据、检查取消，并消耗根 work/I/O；可选的附加 work hook 还可增加限制。窗口必须至少容纳一条记录，否则以资源错误失败。已加载的 read window 自行持有所需 backing，因此其 plan、Result 或 execution context 销毁后仍可使用。动态空集合保留 descriptor support，但不伪造数据行。

Brush state 在调用间携带已定稿 event 前缀和 dab 数。`advance_causal_brush` 消费一个有序 batch，并返回新 state 与本批新增 dabs。End 不可撤销。Work 或资源准入失败会保留之前可用的状态，不发布部分推进结果。每个已发布 Iterative ResultRef 冻结对应 estimate 及关联 system 数据。

## 4. 算法与数学

Spectrum 轴长为 N 并采用 R2CHalf 时，存储索引为 `0..floor(N/2)`，省略部分通过共轭反射定义。原始尺寸留在 schema 中，用来区分奇数和偶数长度。ExactHermitian 直接检查有限实部和虚部。RealProjectionMeasured 将存储值 `a` 与镜像值 `m` 的共轭比较，对缺陷 `d` 使用指定接受范围：

$$
|d| \le atol + rtol\max(|a|,|m|),
$$

其中 `d=a-conj(m)`。纯绝对比较保留大相等值旁边的小差异；相对路径使用分离指数的 binary64 fractions 以避免溢出。该接受规则不是认证 FFT 误差界。Nyquist 列不要求整体为实数；只有所有变换轴均自共轭的点才必须为实数。

Bands v1 使用 duplicate-last Haar：`low=(a+b)/2`、`high=(a-b)/2`；合成时对 low 与 high 相加/相减，再裁剪到记录的 parent shape。五样本输入 `[1,3,5,7,9]` 生成 low `[2,6,9]` 和 high `[-1,-1,0]`，裁掉重复的第六个样本后重建原序列。所需成员为一个最终 `(L,0)`，加上 level 1 到 L 按声明轴顺序的所有非零 mask。ExplicitZero 是有逻辑形状但无存储系数的显式成员，不等同于缺失 key。

Path Bezier 使用 Bernstein 多项式。Hermite `(P0,P1,d0,d1)` 对应 cubic 控制点 `(P0,P0+d0/3,P1-d1/3,P1)`；导数定义在归一化 `t in [0,1]`。Arc 公式为 `c+u cos(theta)+v sin(theta)`。Sweep arc 的 sweep 非零且严格短于一周；full turn 保存带符号方向并从起点闭合。非正交轴仅当向外 determinant 区间证明非退化时有效。零、退化、多周输入需要显式转换；`sin(2*pi)` 不能证明端点相等。

非周期 B-spline 要求足够控制点、非递减 knots、`knots[degree] < knots[control_count]` 和正有理权重。内部 knot 的 multiplicity 为 degree+1 时必须拆分子路径；允许 degree-zero 常数段。零分母项取零，有效域右端取左极限。单独开放 Arc 和端点相等性未确定的 unclamped spline 都可表示；需要未证明端点吸附的混合 primitive 路径失败。Path 属性值跨度完整且不重叠；ArcLength 参数严格覆盖 `[0,1]`。Core Segment 属性索引 verb 记录；primitive Segment 属性索引 PrimitiveRefs。

Component ID 可以是 `1+min_position`，也可以按最小位置顺序紧凑编号。校验精确检查表面积/最小位置及 label 成员关系。四连通 labels 操作遍历左/上边并构造连通划分；导入 labels 的结构校验不证明连通性。

Brush identity 记录 stroke、next event、已定稿 event 前缀和 dab 数、初始/当前 canvas 版本、generation、seed/counter 及 end 状态。Carry 保存 last position、到下个 dab 的剩余距离、预乘 `P[3]`、coverage `A` 和独立 emission `E[3]`。Position 必须有限且非递减；emission 有限且允许带符号；`A` 在 `[0,1]`，且 `A=0` 时 `P=0`。Pending event IDs 与 positions 数量匹配且有界；ended state 不含 pending 项。State consistency 对已定稿 dab 与携带 spacing 的允许差值为 `32*epsilon*max(|a|,|b|,spacing)+32*denorm_min`，其中 `a,b` 是比较的位置。该容差用于状态校验，不是渲染误差界。Helper 执行一维恒定 spacing fold，并跨 batch 携带 phase；不执行 smoothing 或 smudge。Dab vector 使用 Payload allocator role；复制仍受 Payload 子限额约束，增长时旧、新内存块同时计入准入。Spacing 在 binary64 中无法前进时失败，之前状态保持不变。

Iterative identity 包含 system snapshot、初始化、规模、迭代上限和收敛策略。Stop reason 为 `Converged=0` 和 `IterationLimit=1`。零初始化要求 `x0=0`。

$$
r_\infty = \max_i |b_i - \operatorname{round}_{64}(a_i x_i)|.
$$

有限 residual（包括零）不认证解误差。近似消费必须显式选择。乘法先单独舍入，再与 `b_i` 相减。Schema/content 校验与 brush stepping 会建立最近偶数舍入和渐进下溢环境，并恢复调用方浮点环境。

## 5. 限制与非目标

- YCbCr420 记录 full-range Y `[0,1]`、Cb/Cr `[-0.5,0.5]`、BT.709 transfer、sRGB D65 原色和声明的 scene/display reference。Chroma extent 为半尺寸向上取整，名义 origin `(0.5,0.5)`、step `(2,2)`；边缘裁剪后的 source support 单独计算。该 schema 不是 I/O codec。内部图像平面遵循同尺寸 planar 契约。
- PathSet 不从几何容差推导拓扑、布尔路径正确性或抗锯齿误差。周期 spline seam 校验不属于该 schema。
- Components 结构校验不证明任意导入 labels 的连通性。
- Brush state 可表示有界 pending/lookahead events，但公开 helper 只计算因果恒定 spacing 行为；不实现 smoothing 或 smudge。
- Iterative 实测 residual 不是 CertifiedBound。
- Managed work、I/O、窗口和容量限制不构成进程 RSS 上界。
