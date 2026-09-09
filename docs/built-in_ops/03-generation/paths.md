# 路径、变化线宽与光栅化

状态Proposed。固定Bézier数学为D1；复合PathSet、通用stroke/fill/boolean为D2。当前Value只有一个数组输出，变长路径及关联属性需要G3；不能把“能保存rank4数组”当成已有矢量数据模型。

## 表示

固定次数首版：quadratic controls `[S,3,2]`，cubic controls `[S,4,2]`，每段固定样本 `[S,N,2]`。复合路径建议controls/verbs/control_offsets/path_offsets/closed分别保存；move/line/quadratic/cubic/close为首批，arc、布尔和字体轮廓后续。公共端点是否重复、闭合是否隐含末段、每段属性归属必须固定。SVG2提供路径与子路径的公开语义词汇。[^svg]

坐标采用[公共约定](../00-foundation/contracts.md)，width单位px，明确表示直径。全op共享一个width profile符合首期需求，但profile自变量必须选择整条路径归一弧长、每段t或实际弧长，建议整路径弧长`s/L`。

## 算子目录

| ID / 功能 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| PTH-01 make/concat/split/reverse | controls+topology→PathSet | 闭合/接点显式；不自动吸附邻近端点 | 段方向、索引、属性一一对应 |
| PTH-02 evaluate | path+t→position/tangent | 首版Bézier使用de Casteljau，t∈[0,1]；B-spline须另带degree/knots | 端点与解析直线；零导数不归一成NaN |
| PTH-03 arc length | path→length及(t,s)表 | 自适应积分/细分；误差px | 直线长度、可复核曲线数值积分 |
| PTH-04 resample | path+spacing/count→points | uniform_t/uniform_arc/adaptive；数量上界 | 相同弧长位置与控制点密度无关 |
| PTH-05 width profile | path+scalar curve→widths/path | linear默认，PCHIP可选；w≥0，整op共享 | 0、亚像素、非单调宽度、端点 |
| PTH-06 transform | path+matrix→path | centerline变换后描边或原stroke轮廓整体变换分开 | 非均匀缩放两者应有不同明确结果 |
| PTH-07 trim/dash | path+interval/pattern→path | 弧长px/归一长度、offset、closed seam | 跨段/接缝节奏连续 |
| PTH-08 fit/simplify/smooth | samples/path→path | error tolerance、保尖角/拓扑、最大段数 | 最大几何误差而非只看点数下降 |
| PTH-09 fill | path+canvas→coverage | nonzero默认，evenodd可选，AA模式 | 自交/孔洞/反向子路径/边缘面积 |
| PTH-10 stroke | path+width+canvas→coverage | round cap/join默认；butt/square/miter/bevel，miter limit必填 | 自交、尖角、重叠、极端miter、0width |
| PTH-11 distance | path→unsigned field或SDF | 开放中心线仅unsigned；signed必须来自按fill rule定义的区域或width/cap/join构成的stroke | inside<0、px单位；与解析/穷举比较，closest多解规则 |
| PTH-12 boolean/offset | PathSets→PathSet | union/intersect/difference/xor，fill与容差 | 拓扑、几何退化、极小缝；D2 |

Bézier二次 `B=(1-t)²P0+2(1-t)tP1+t²P2`；三次用对应Bernstein权重。弧长 `s(t)=∫₀ᵗ|B′(u)|du`，建议建立单调(t,s)表后二分/保护迭代反求t。均匀t通常不是均匀几何距离；Fourier只适合显式周期profile，普通线宽优先linear/PCHIP避免振铃负值。

整路径L=0时不计算s/L：参考mode令profile坐标为0，arc resample返回显式有效单点或按输出数量重复该点，零长stroke的cap政策单独规定（round可定义圆点，butt为空）。当前空shape限制仍按G3处理。后期B-spline需degree/knots及归一t到有效knot区间的映射，不能仅复用Bézier控制点数组。

## 光栅化方案

CPU参考可采用自适应细分+扫描线/边表+coverage累计；建议预览曲线几何误差.25px、高质量.05px作为待测起点，coverage积分精度另定。逐像素遍历全部段成本O(P·S)，需要空间桶/BVH优化。GPU可研究细分三角形或Loop–Blinn解析曲线判定，它并不自动解决任意拓扑、变化线宽和准确面积积分。[^loop]

coverage与SDF独立：前者为像素面积比例[0,1]，后者为px距离。中心距离smoothstep只是coverage近似；细线、尖角和高曲率需面积基准。`min/max`组合SDF通常只保留部分符号/边界性质，不保证仍是精确距离；继续offset前可能需要重新距离化。

同一stroke自重叠建议先合并几何coverage再着色，避免每个细分片重复source-over导致深色接缝；若工具目标是按笔迹累积flow，则命名为有序paint过程。CSP线宽的加减与比例缩放对渐尖端点不同，应提供独立控制。[^csp]

## Region、使用和验收

输出tile需读取所有可能覆盖其footprint的段，stroke扩张包含半宽、join/miter与AA。当前没有这种PathSet专用demand，首版可读完整控制点表，输出策略仍需G3/G4。路径中一次编辑的dirty不能只用控制点bbox，要覆盖曲线与描边的真实/保守范围。

概念流程：`controls→arc-length→width curve→stroke coverage→color ramp/constant→over`；另可`path→SDF→mask offset→feather`。解析验收包括100px直线、圆/矩形面积、孔洞、自交、闭合接缝、宽度0/.25/1/10px、非均匀变换和不同tile一致。边界与迭代使用Float64参考，GPU coverage采用明确误差而非未经证明的bit identity。

## 来源

[^svg]: W3C，[*SVG2 Paths*](https://www.w3.org/TR/SVG2/paths.html)，2018-10-04 CR；路径动作/子路径。
[^loop]: Loop、Blinn，[*Resolution Independent Curve Rendering using Programmable Graphics Hardware*](https://www.microsoft.com/en-us/research/publication/resolution-independent-curve-rendering-using-programmable-graphics-hardware/)，SIGGRAPH2005。
[^csp]: CELSYS，[*Vector layers*](https://help.clip-studio.com/en-us/manual_en/180_layers/Vector_layers.htm)，滚动手册；线宽编辑方式。
