# 抠像与 matte 基础算子

本页为基础算子的中文目录，`op_specs/` 下的英文规格是权威定义。图层、图层组与蒙版使用行为由这些算子显式组合。

## 基本术语与数值规定

- 全部成员为 tile 算子；数学依赖仍区分逐像素、有限邻域、全图与显式帧组。ROI／tile 边界不改变算法问题。
- 生产计算采用原生 FP32/FP64；严格舍入 oracle 用于数值比较，复制、标签和离散选择单独检查。
- 颜色解释是显式静态参数，数值域遵循[公共 unchecked 契约](../00-foundation/contracts.md)。
- 线性光与编码 RGB 的覆盖合成使用独立成员；颜色混合、alpha 合成和 mask 应用分别组合。
- CIELAB 明度为 l=L*/100，OKLab 使用原生 L；两者色度单位不同。

## 族目录

| 族 | 功能 | 成员 |
| --- | --- | --- |
| KEY-01 范围选区 | 标量范围与柔和过渡 | [A](op_specs/KEY-01A_range_select.md) |
| KEY-02 色键 | 完整 Lab 距离与忽略明度的色度距离 | [A](op_specs/KEY-02A_lab76_chroma_key.md), [B](op_specs/KEY-02B_lab_chroma_key.md) |
| KEY-03 差分抠像 | 对齐图像与背景的 RGB 距离 | [A](op_specs/KEY-03A_difference_rgb_l2.md) |
| KEY-04 背景估计 | 单图最近供体与已对齐多帧中值 | [A](op_specs/KEY-04A_clean_plate_nearest.md), [B](op_specs/KEY-04B_clean_plate_temporal_median.md) |
| KEY-05 Trimap matting | 硬约束闭式抠像数学 | [A](op_specs/KEY-05A_closed_form_matting_clipped.md) |
| KEY-06 前景估计 | 显式背景／先验的正则颜色估计 | [A](op_specs/KEY-06A_foreground_regularized.md) |
| KEY-07 Matte 细化 | 腐蚀、膨胀、二值填洞及引导细化 | [A](op_specs/KEY-07A_matte_erode.md), [B](op_specs/KEY-07B_matte_dilate.md), [C](op_specs/KEY-07C_binary_fill_holes.md), [D](op_specs/KEY-07D_matte_guided_gray.md) |
| KEY-08 去溢色 | 绿色超量与指定色度方向抑制 | [A](op_specs/KEY-08A_despill_green_excess.md), [B](op_specs/KEY-08B_despill_directional.md) |
| KEY-09 边缘延色 | 保持目标 alpha 的原始供体颜色延展 | [A](op_specs/KEY-09A_edge_extend_nearest.md) |

## 逐成员入口

共 15 个 Proposed 成员。

| 成员 | 类别／执行／成熟度 | 提议注册 key | 实现状态 |
| --- | --- | --- | --- |
| [KEY-01A Soft scalar range selection](op_specs/KEY-01A_range_select.md) | primitive / tile / D1_draft | `keying.range_select` | not_implemented |
| [KEY-02A CIELAB full-color distance key](op_specs/KEY-02A_lab76_chroma_key.md) | primitive / tile / D1_draft | `keying.lab76_chroma_key` | not_implemented |
| [KEY-02B CIELAB chroma-only distance key](op_specs/KEY-02B_lab_chroma_key.md) | primitive / tile / D1_draft | `keying.lab_chroma_key` | not_implemented |
| [KEY-03A RGB difference key](op_specs/KEY-03A_difference_rgb_l2.md) | primitive / tile / D1_draft | `keying.difference_rgb_l2` | not_implemented |
| [KEY-04A Nearest known background plate](op_specs/KEY-04A_clean_plate_nearest.md) | primitive / tile / D1_draft | `keying.clean_plate_nearest` | not_implemented |
| [KEY-04B Background plate from aligned frames](op_specs/KEY-04B_clean_plate_temporal_median.md) | primitive / tile / D1_draft | `keying.clean_plate_temporal_median` | not_implemented |
| [KEY-05A Trimap-constrained closed-form matting](op_specs/KEY-05A_closed_form_matting_clipped.md) | primitive / tile / D1_draft | `keying.closed_form_matting_clipped` | not_implemented |
| [KEY-06A Regularized foreground color estimate](op_specs/KEY-06A_foreground_regularized.md) | primitive / tile / D1_draft | `keying.foreground_regularized` | not_implemented |
| [KEY-07A Matte erode](op_specs/KEY-07A_matte_erode.md) | primitive / tile / D1_draft | `keying.matte_erode` | not_implemented |
| [KEY-07B Matte dilate](op_specs/KEY-07B_matte_dilate.md) | primitive / tile / D1_draft | `keying.matte_dilate` | not_implemented |
| [KEY-07C Fill enclosed binary holes](op_specs/KEY-07C_binary_fill_holes.md) | primitive / tile / D1_draft | `keying.binary_fill_holes` | not_implemented |
| [KEY-07D Gray-guided matte refinement](op_specs/KEY-07D_matte_guided_gray.md) | primitive / tile / D1_draft | `keying.matte_guided_gray` | not_implemented |
| [KEY-08A Suppress green excess](op_specs/KEY-08A_despill_green_excess.md) | primitive / tile / D1_draft | `keying.despill_green_excess` | not_implemented |
| [KEY-08B Suppress a specified chromatic spill direction](op_specs/KEY-08B_despill_directional.md) | primitive / tile / D1_draft | `keying.despill_directional` | not_implemented |
| [KEY-09A Extend original edge colors](op_specs/KEY-09A_edge_extend_nearest.md) | primitive / tile / D1_draft | `keying.edge_extend_nearest` | not_implemented |

## 相关入口

- [共通规格](op_specs/COM_common_contract.md)
- [研究来源](research-sources.md)
- [严格舍入 oracle](../../../oracle/ops/09-composite/README.md)
- [混合与覆盖合成](blending.md)

亮度范围选区使用 FMT 的 Y／CIELAB l／OKLab L 提取与 KEY-01A 组合。KEY matting 的数学定义见[独立 KEY 契约](op_specs/KEY_matting_contract.md)。
