# 数值与数组基础

已接受首版见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)：四 dtype cast/range 分离，Float32/64 同 shape 基础算术和显式 clamp，全数组 Float64 mean/variance，有界单通道 expression（静态 start/step/count、动态 Float64 coefficients）及 linear 1D LUT。默认由 workflow 显式提交，全部新增算子首版 Whole；其他 unary/broadcast/scan/数组操作继续 Proposed。

当前已实现的 cast/encode_range、四种二元算术、clamp、mean/variance 及公开运行示例见[数值算子实现](../../kernel-architecture/zh/Numeric-Operations.zh.md)。Expression/LUT 在后续切片交付；下表扩展目录仍为 Proposed。

状态 Proposed。本篇为 D1 数学核心，受 G1/G3/G5 数据与组合前置条件约束。建议 CPU Float32/Float64 参考实现；整数支持逐项定义，不能默认为所有张量运算有 Metal 后端。符号 E/W/S 和默认约定见[公共契约](../00-foundation/contracts.md)。

## 基础目录

| ID / 提议操作 | 输入 → 输出 | 参数、语义与算法 | 依赖 / 验收 |
| --- | --- | --- | --- |
| NUM-01 `numeric.sample_expression` | 表达式+domain → `[N]` 或 `[N,C]` | `x0=0, step>0, count>=1`；`x_i=x0+i·step`，默认 Float32，binary64 求值后转出；显式 end 模式与 count 互斥；E | G3；`2*x+1,x0=0,step=.25,count=5` 得 `[1,1.5,2,2.5,3]` |
| NUM-02 `numeric.linspace` / `arange` | start/end/count 或 start/step/count → `[N]` | linspace 默认含两端，N=1 返回 start；arange 采用 count 保证长度确定，避免浮点循环停机差异；E | 几何、曲线、噪声坐标基础 |
| NUM-03 `numeric.constant` / `broadcast` | scalar/array + shape → array | 显式 broadcast axes，拒绝模糊的隐式通道广播；可用只读零 stride view；E | 注册 Fixed 与按节点输出 shape 的边界须核验 |
| NUM-04 `numeric.unary` | array → 同形 | abs/neg/sqrt/exp/log/pow/sin/cos/tan/floor/ceil/round/sign/reciprocal；定义域错误默认 reject；E | `sqrt([-1])` 必须按策略报错，round 建议 ties-even |
| NUM-05 `numeric.binary` | A,B → 对齐 shape | add/sub/mul/div/min/max/pow/atan2；shape 严格或显式 broadcast；E | 除零、signed、overflow；整数安全策略显式 |
| NUM-06 `numeric.clamp` / `remap_range` | array → 同形 | `[a,b]→[c,d]`，`a<b`，默认不夹紧；clamp 独立；E | .5 从 [0,1] 映到 [0,255] 得127.5 |
| NUM-07 `numeric.compare` / `select` | A,B 或 condition,A,B → bool mask/array | eq/ne/lt/le/gt/ge；近似相等独立 atol/rtol；select 不混同颜色 blend；E | NaN 比较规则、mask role |
| NUM-08 `numeric.mix` / `smoothstep` | A,B,t / field,e0,e1 → array | mix=(1-t)A+tB，t 默认 [0,1]；smoothstep 使用有序不同端点、三次 Hermite；E | t=0/1 identity，e0=e1 reject |
| NUM-09 `array.reshape` / `transpose` / `slice` | array → array | 元素数守恒；轴置换/起点步长明确；优先不可变 view；R | 非连续、负 stride 与逻辑 Region；不隐式改变颜色角色 |
| NUM-10 `array.concatenate` / `gather` / `scatter_reduce` | arrays/indices → array | axis、索引域、重复索引；scatter 必须定义 sum/min/max/replace 和确定顺序；W/R | 索引越界、重复 index、内存上界 |
| NUM-11 `numeric.reduce` | array + axes → reduced array | sum/min/max/mean/count/variance/std；ddof=0、Float64 累加建议；W | `[1,2,3]` mean2、population variance2/3 |
| NUM-12 `numeric.quantile` / `sort` | array → array | quantile 算法明确，例如相邻 order statistic 线性插值；axis、stable；W | .5 与 median 一致；NaN策略不隐含 |
| NUM-13 `numeric.prefix_sum` / `integral_image` | array → prefix | axis；exclusive 建议多一个起始零；累加Float64；S | `[1,2,3]→[0,1,3,6]`，矩形四角查询 |
| NUM-14 `numeric.matrix_transform` | vectors + matrix/bias → vectors | 行列序、左右乘明确；2D/3D/4D，小矩阵CPU SIMD/GPU；E | 单位矩阵 identity；奇异逆变换报错 |
| NUM-15 `numeric.derivative_1d` / `integrate_1d` | sampled values+step → values | central difference / trapezoid，端点策略，单位随 step 改变；H/S | f(x)=x² 的内部导数2x，常数积分 |

以上是基础语义设计，不声称有单个商业软件与每行完全对应。数据配线、数学和统计通常用于构造上层流程，不应全部暴露为面向绘画使用者的主菜单。

## 表达式生成器

输入至少还需要定义域和样本数，只有“一维表达式、采样间隔”无法确定有限输出。优先将 `start,step,count` 作为规范形式，允许 UI 的 `start,end,step` 转换后显示实际终点。支持显式 `x`、数值常量和白名单纯函数，控制 AST 深度、节点数、表达式长度及求值成本；不执行任意脚本或文件 I/O。非有限结果附样本索引失败，不能悄悄产生合法 image facet。

建议静态：表达式 AST、样本布局、输出 dtype、定义域决定 shape 的参数。动态：不改变 shape 的系数可作为 Value 输入。N、C 与输出推断属于 G3；先实现固定/显式描述的最小版本，再决定可复用节点形状机制。

为 LUT 提供采样时，输出同时需要 x-domain，不能只给裸数组让下游猜 `[0,1]`。为路径宽度提供值时，独立变量应明确为 segment t、全路径弧长或归一化弧长。这里将需求中的 “spine” 暂按沿路径变化曲线理解；若指骨骼 Spine 动画资产，则属于不同数据模型。

## 实现和使用面

逐元素族建议共享循环、dtype dispatch、边界检查和向量化设施，同时保留各操作独立语义与参数校验。矩阵、scan、reduce 可复用经验证的数值库，但库的默认 NaN、rounding、归一化与线程策略必须适配。全局归约先用固定遍历或固定树，避免无序 atomic 造成结果漂移。

概念工作流：`sample_expression → LUT apply`、`linspace → periodic noise → displacement field`、`channel extract → mean/variance → auto exposure`、`path arc length → width curve`。这些流程暂不是现有 registry 的可运行代码；落地时必须补公开入口最小样例。
