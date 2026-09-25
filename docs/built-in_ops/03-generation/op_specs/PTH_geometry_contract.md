---
spec_schema_version: 1
id: PTH-geometry
kind: shared_operator_contract
category: 03-generation
status: AcceptedDesign
implementation_status: not_implemented
registration_status: technical_freeze_gates
---
# 路径几何、关联、误差与规范化

继承 [GEN-common](GEN_common_contract.md) 与现有
[Structured representations](../../../kernel-architecture/Structured-Representations.md)。
PathSet v1 已是存在的 schema；节点并未因此实现。

## geometry authority 和属性

CoreVerbs: M/L/Q/C/Z arity=1/1/2/3/0。单个 M 是合法零长 open 子路径；
Z 是终结 verb，与 closed 一致；Z 的闭合线是实际段，不再生成第二份 implicit close。
空集合 verb/control/subpath rows 均0，两个 offsets 均[0]。
Primitive authority 的 CoreVerbs 必须为空，primitive引用、payload字段及 ObjectId 一一匹配。
坐标有限 Float64，默认 pixel-xy-right-down；最大段1,048,576，控制点4,194,304，spline degree≤16。
这些是被检查的 representation.hpp 上限，不以 Python list 构造另一个公共 Path 类型。

拷贝几何产生新 Result 时重建全部 association，不保留指向旧 ObjectId 的 primitive row。
concat 按输入端口顺序→子路径原序，split 返回独立 Result 不能输出 Result 内嵌 Result。
需要不定个数 split 的成员返回一个扁平 PathSet 与 partition 表，而非运行时增减端口数。

属性域 Path/Subpath/Segment/Control/ArcLength、插值 Constant/Linear 是当前 schema 的能力。
几何重建时只有明确可映射的属性可传播；默认 `attribute_policy=reject_unmappable`，
显式 drop 才可删除不能映射的属性，必须记录丢弃名单。无语义名称的属性不猜成 width。
宽度绑定显式区分整PathSet归一化、逐子路径归一化、逐子路径实际弧长；不静默复用旧ArcLength标签。
整组长度不计子路径跳跃；零长、接缝和域边界随正式schema冻结。
width 是直径、非负、单位 px；PCHIP 作为查询计算，不伪称 schema v1 的属性插值 enum 支持 PCHIP。

## 求值与规范化边界

Bézier 对绝对控制点用完整 Bernstein/de Casteljau 数学式一次 RN 到输出；不逐轮舍入。
这与 CRV-03 的 relative handles 重建 RN64 是不同输入合同，桥接时须显式执行那一步。
归一 t∈[0,1]；端点直接返回控制点位。导数输出是 dP/dt，不归一化，零导数=(+0,+0)。
要求单位切线时需单独派生 validity，不能零除出 NaN。
Arc: `P=c+u*cos(theta0+t*sweep)+v*sin(...)`；FullTurn有明确闭合端点选择，
不能靠 sin(2*pi)≈0 判连接。B-spline 用有效区间 [k[d],k[n]]，右端取左极限。

Core 局部 t 查询：segment ID 是 verb row，M不是可求值段，Z表示闭合线；primitive 查询用引用 row。
离散 segment selection 精确；拼接处全局弧长查询除终点外选后一个正长段，零长段跳过。
整条长度0：有起点则有效单点；真正空集合按成员的 NoSolution/空 Result 规则区分。

## 弧长和误差报告

`s(t)=integral_0^t ||P'(u)|| du`。直线解析；Bézier 可用 dyadic subdivision 的 chord 下界、
control-polygon 上界，长度求和使用有向包络。普通 Gauss–Kronrod 差值仅是估计，不能自动标 CertifiedBound。
严格 length 数值要求 RN64 真长度；不能仅因 |upper-lower|≤epsilon 就停止并宣称正确舍入。
可先按 epsilon 建表，再额外 refine 到同一 rounding cell；midpoint ambiguity 必须继续或证明 exact tie。

逻辑新增 ArcLengthTableV1 Result：`segments`(Int64 sourceID)、`ranges`(Int64 begin/end)、
`knots` Float64(t,s_lower,s_upper)、`totals` Float64(length,length_lower,length_upper)；
每个字段与原 path snapshot/整表 ObjectId 关联、CompleteBundle、递增 t，s bounds 非减。
正式 factory/codec 需另审；本次只提交规格，不新增运行时 schema。

## flatten-v1 的确定义（非原曲线精确化）

仅对 Bézier（Hermite 可精确转换为有理控制后处理）。递归以精确 t=1/2 de Casteljau 分割，
从左到右深度优先。对 degree d 的控制点 P_i，用同参数弦的 degree-d controls
`Q_i=(1-i/d)P0+(i/d)Pd`；若每个 `||P_i-Q_i||²≤epsilon²` 则接受该弦。
该 convex-hull 界证明参数对应距离≤epsilon，**不是只检查垂距**，因此不会漏掉共线回折。
所有判断用精确/有向包络；每个原段最大深度默认24、最大输出段1,048,576。
叶端点先 RN64；发布舍入的最大位移须计入预算，所用几何是这些实际发布的线段。
没有余量容纳舍入时继续细分不一定有用，应拒绝 InvalidQuality。

epsilon_geom_px默认0.05，显式预览0.25，均为目标画布像素，不是 NUM 数值容差，也不是 .05 的 coverage误差。
strict 对**规范化折线**的面积/距离正确舍入，几何 certificate 分开说明相对于源曲线的界。
需要原曲线像素面积的用户不能把该成员当 exact-curve；未来另具名解析/有界积分成员。
微孔/自交拓扑不由参数距离界保证；要求保拓扑时必须单独证明，不能用“足够小epsilon”。

## 填充、布尔与距离

填充把 open subpath 在消费时用一条线闭合，不修改原对象；单点/零面积为零 coverage。
nonzero默认，evenodd显式。inside 的 ray crossing 采用半开规则；边界归属不影响面积。
布尔采用 regularized 2D 区域：孤立点、零面积线不作为面返回；端点保留只属于 centerline。
几何谓词精确；同一点/共线/交叉不得由四ULP容差替代。

距离必须到所选区域的**实际边界**，不把重叠多边形的内部消失边计算进去。
例如两矩形 union 后的 SDF 不能简单 min 两个原 SDF 即称 exact。
closest 多解按(source subpath,segment,t)字典序，t取较小者；布尔生成边按规范化边序。
空 region：截断SDF成员返回显式 +max_distance；非截断 unsigned 最近点返回 NoSolution。

## 描边

width=0→空覆盖（不隐式 hairline）。round/butt/square cap，round/bevel/miter join；
miter_limit≥1，按外角尖点距中心/半宽定义，超限回落bevel，相等使用miter。
路径的自重叠先做几何 union 再求一次coverage；不是多个patch source-over。
单 M 或全零長段：round→半径w/2圆盘，square→以该点为中心的轴对齐w×w方形，butt→空。
closed 没有 end cap；反向尖点的 round/bevel/miter退化必须沿上面的区域 union 构造。
变化宽度的明确基准在 PTH-10B；不能把 constant radius 的曲线 offset 公式直接套上。

## 误差、空间索引与资源

光栅输出为Regional，全路径可作为完整验证及Conservative控制依赖；BVH仅可裁掉已证明不影响选定输出的候选。
stroke dirty bbox 包含半宽、square cap、miter延伸、AA和geometry error，而非裸控制点bbox。
长度/归一属性改动可能全局影响。P/S/K/B 分别像素、段、候选事件、backing记录量；
交叉最坏O(S²)，动态行数不可预先假称O(S)。准入要检查事件/页/验证工作预算。

来源：[S02](../research-sources.md#s02)、[S03](../research-sources.md#s03)、
[S14](../research-sources.md#s14)、[S15](../research-sources.md#s15)、[S17](../research-sources.md#s17)。

## 正式扩展约束

宽度支持标准附着与独立输入，恰好一个显式来源；默认reject_unmappable，显式drop_unmappable报告键和原因。
trim/dash保留源位置宽度；重新铺满片段须显式rebind。近似及拟合默认allow_change，可选reject_unproven；
未验证不得报告已保持或已改变。精确/网格Boolean及精确Result/Float64输出分别定义，见PTH-12族。
不同拟合后端是独立算子，不能以运行时solver切换隐藏结果变化。
