# Layer 值（合成算子 SDK）

英文权威文档：[Layer-Runtime.md](../Layer-Runtime.md)。

本页说明算子 SDK 中的类型。Layer 值及其纯算术 helper 声明在 `photospider/ops/composite/layer.hpp`，实现在 `plugins/ops/09-composite/layer.cpp`，并构建进 `Photospider::ops_sdk`。内核没有 Layer 类型、Layer schema 或 Layer 专用校验。Workflow 中的图像使用 [Result Tensor 存储与区域访问](../../kernel-specs/zh/Tensor-Storage-and-Region-Access.zh.md)；`Tensor` 提供统一数值 backing。

## 1. 模块边界与所有权

合成 SDK 拥有内存中的值契约与纯计算。该头文件不暴露算子 factory，也不暴露 Result schema。`LayerPixel` 直接传给值 helper；在 Result 中存储 Layer 数据的算子自行选择 tensor 布局，并以显式参数传递任何解释。

## 2. 数据结构与内存

```cpp
struct CoveragePixel final { std::array<float, 3> p{}; float a = 0; };
struct LayerPixel final { CoveragePixel coverage; std::array<float, 3> emission{}; };
struct LayerResponsePixel final { std::array<float, 3> q{}; float t = 1; };
struct RawRgbaSumPixel final { std::array<float, 3> p{}; float mass = 0; };
struct WeightedLayerSum final { std::array<double, 8> components{}; };
struct LayerContribution final { std::array<double, 8> components{}; };
struct OptionalLayerPixel final { bool valid = false; LayerPixel value; };

Status validate_coverage(const CoveragePixel&);
Status validate_layer(const LayerPixel&);
Status validate_raw_rgba_sum(const RawRgbaSumPixel&);
Status validate_weighted_layer_sum(const WeightedLayerSum&);
Status validate_layer_contribution(const LayerContribution&);
```

`CoveragePixel` 保存关联颜色 `P` 与覆盖率 `A`；`P` 的每个分量都有限，`A` 有限且位于 `[0,1]`，`A=0` 时要求 `P=0`。`LayerPixel` 增加独立、有限、可带符号的发射 `E`；七个分量构成一个不可分割的观测，处于调用方选择的同一个线性颜色空间中。Helper 不命名也不转换该空间。Response 保存有限的 `Q=P+E` 与位于 `[0,1]` 的 `T=1-A`，因此无法恢复原始的覆盖/发射拆分。

Raw sum 保存有限 `P` 与有限非负的可加质量 `M`；`M=0` 时要求 `P=0`，质量不是透射率。Contribution 保存 binary64 的 `P,A,E,w`，前七个值从 binary32 精确提升，`w>=0` 且有限。已发布的 weighted sum 保存 `Np[3],Na,Ne[3],W`；要求所有值有限、`W>=0`、`0<=Na<=W`、`Na=0 => Np=0`，以及 `W=0 => 所有分子为零`。内部归约 scratch 可以暂时违反关联关系。`valid=false` 的 optional Layer 表示空的加权观测，不表示黑色。校验函数对非法的导入关联返回 `TypeMismatch`。

## 3. 执行与状态机

```text
Layer value structs --pure helpers--> Layer / Response / RawSum values
                                   \
                                    +--> failure Status; inputs unchanged
```

Helper 是同步、线程安全的函数，独立于图像执行返回本地值。错误对调用方可见。

## 4. 算法与数学

纯 helper 校验输入并返回新值，不修改输入。其 binary32 原语使用就近偶数舍入、渐进下溢、不做 FMA 收缩，并在每个规定运算后单独舍入。它们会恢复调用方的浮点环境。非有限结果和关联下溢返回带稳定 reason 的 `OperationFailed`；helper 不发布部分值。

`layer_over(front,back)` 计算 `T=round32(1-Af)`，并分别舍入 `P=Pf+T*Pb`、`A=Af+T*Ab`、`E=Ef+T*Eb`。`layer_opacity` 要求有限且位于 `[0,1]` 的 opacity，缩放 `P` 与 `A`，保留 `E`。`layer_emit` 要求有限的 gain 与 emission，二者都可带符号。前方发射增加 `gain*Eg`；后方发射增加 `T*(gain*Eg)`。`layer_flatten` 使用显式不透明背景 `B` 计算 `(P+E)+(1-A)*B`，并返回不透明覆盖。`response_over` 计算 `Qf+Tf*Qb` 与 `Tf*Tb`。

`raw_rgba_plus` 相加 `P` 与 `A` 并累计非负质量。`raw_rgba_coverage` 要么要求 `M<=1`，要么在保留 `P` 的同时把 alpha 限为 `min(M,1)`；它从不除以质量。加权 leaf 与 add helper 执行 binary64 算术，并校验每个已发布的中间值。`weighted_layer_finalize` 用 `W` 除每个分子，舍入到 binary32，再校验完整 Layer。`W=0` 时不做除法，返回 `valid=false`。

纯 `weighted_layer_leaf` 与 `weighted_layer_add` helper 不选择累加树。组合 leaf 的调用方自行选择并负责分组策略。

## 5. 限制与非目标

- 不提供 Layer 算子 factory、Layer Result schema 或 Layer 图像 workflow。内核不识别也不拒绝任何以 Layer 命名的 schema id；schema id 只是结构名称。
- Layer 值不定义 Result Tensor 存储、Region 访问、分块图像执行或 GPU 后端。
- 算术契约不提供认证的数值误差界，也不提供进程 RSS 上界。
- Helper 不转换颜色空间，也不从样本值推断工作空间。调用方处理 helper 错误，包括非法关联、算术溢出和加权关联下溢。

[混合与合成](../../built-in_ops/09-composite/blending.md)中的合成算子目录说明 bundled operations 与这些类型的关系。
