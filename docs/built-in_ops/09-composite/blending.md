# 混合与覆盖合成

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
| BLN-01 Normal | 源颜色选择 | [A](op_specs/BLN-01A_normal.md) |
| BLN-02 加减除 | 颜色数值运算 | [A](op_specs/BLN-02A_add.md), [B](op_specs/BLN-02B_subtract.md), [C](op_specs/BLN-02C_divide.md) |
| BLN-03 Multiply | 有界与无界乘法 | [A](op_specs/BLN-03A_multiply.md), [B](op_specs/BLN-03B_multiply_unbounded.md) |
| BLN-04 变暗／变亮 | 逐通道极值 | [A](op_specs/BLN-04A_darken.md), [B](op_specs/BLN-04B_lighten.md) |
| BLN-05 Screen | 有界与无界滤色 | [A](op_specs/BLN-05A_screen.md), [B](op_specs/BLN-05B_screen_unbounded.md) |
| BLN-06 Overlay | 按背景分段 | [A](op_specs/BLN-06A_overlay.md) |
| BLN-07 Hard Light | 按源颜色分段 | [A](op_specs/BLN-07A_hard_light.md) |
| BLN-08 Soft Light | W3C 柔光 | [A](op_specs/BLN-08A_soft_light_w3c.md) |
| BLN-09 Color Dodge | 颜色减淡 | [A](op_specs/BLN-09A_color_dodge.md) |
| BLN-10 Color Burn | 颜色加深 | [A](op_specs/BLN-10A_color_burn.md) |
| BLN-11 Linear Dodge/Burn | 有界线性减淡／加深 | [A](op_specs/BLN-11A_linear_dodge.md), [B](op_specs/BLN-11B_linear_burn.md) |
| BLN-12 Linear Light | 线性光 | [A](op_specs/BLN-12A_linear_light.md) |
| BLN-13 Vivid Light | 亮光 | [A](op_specs/BLN-13A_vivid_light.md) |
| BLN-14 Pin Light | 点光 | [A](op_specs/BLN-14A_pin_light.md) |
| BLN-15 Difference/Exclusion | 差值与排除 | [A](op_specs/BLN-15A_difference.md), [B](op_specs/BLN-15B_exclusion.md) |
| BLN-16 Hard Mix | 两种独立阈值定义 | [A](op_specs/BLN-16A_hard_mix_vivid.md), [B](op_specs/BLN-16B_hard_mix_sum.md) |
| BLN-17 整色变暗／变亮 | 按 Y、OKLab L 或 CIELAB l 选择整个颜色 | [A](op_specs/BLN-17A_darker_color_linear_y.md), [B](op_specs/BLN-17B_lighter_color_linear_y.md), [C](op_specs/BLN-17C_darker_color_oklab_l.md), [D](op_specs/BLN-17D_lighter_color_oklab_l.md), [E](op_specs/BLN-17E_darker_color_cielab_l.md), [F](op_specs/BLN-17F_lighter_color_cielab_l.md) |
| BLN-18 Dissolve | 坐标稳定的随机覆盖选择 | [A](op_specs/BLN-18A_dissolve_counter64.md) |
| BLN-19 颜色加光 | 保留背景 alpha 的线性颜色相加 | [A](op_specs/BLN-19A_emission_add_straight.md) |
| BLN-20 图像插值 | 线性／编码域的关联颜色与 alpha 插值 | [A](op_specs/BLN-20A_premul_mix_straight_linear.md), [B](op_specs/BLN-20B_premul_mix_straight_encoded.md) |
| CMP-01 清空 | 生成零图像 | [A](op_specs/CMP-01A_clear.md) |
| CMP-02 输入选择 | 完整复制源或背景 | [A](op_specs/CMP-02A_source.md), [B](op_specs/CMP-02B_destination.md) |
| CMP-03 Over | 普通与预先混色的覆盖合成 | [A](op_specs/CMP-03A_source_over_linear.md), [B](op_specs/CMP-03B_destination_over_linear.md), [C](op_specs/CMP-03C_source_over_blended_color_linear.md), [D](op_specs/CMP-03D_source_over_encoded.md), [E](op_specs/CMP-03E_destination_over_encoded.md), [F](op_specs/CMP-03F_source_over_blended_color_encoded.md) |
| CMP-04 In | 交集覆盖 | [A](op_specs/CMP-04A_source_in_linear.md), [B](op_specs/CMP-04B_destination_in_linear.md), [C](op_specs/CMP-04C_source_in_encoded.md), [D](op_specs/CMP-04D_destination_in_encoded.md) |
| CMP-05 Out | 非重叠覆盖 | [A](op_specs/CMP-05A_source_out_linear.md), [B](op_specs/CMP-05B_destination_out_linear.md), [C](op_specs/CMP-05C_source_out_encoded.md), [D](op_specs/CMP-05D_destination_out_encoded.md) |
| CMP-06 Atop | 保留指定输入覆盖 | [A](op_specs/CMP-06A_source_atop_linear.md), [B](op_specs/CMP-06B_destination_atop_linear.md), [C](op_specs/CMP-06C_source_atop_encoded.md), [D](op_specs/CMP-06D_destination_atop_encoded.md) |
| CMP-07 Xor | 对称非重叠覆盖 | [A](op_specs/CMP-07A_xor_linear.md), [B](op_specs/CMP-07B_xor_encoded.md) |
| CMP-08 Plus | 原始颜色质量／权重与有界加法 | [A](op_specs/CMP-08A_plus_raw_linear.md), [B](op_specs/CMP-08B_plus_lighter_bounded_linear.md), [C](op_specs/CMP-08C_plus_raw_encoded.md), [D](op_specs/CMP-08D_plus_lighter_bounded_encoded.md) |
| NBL-01 色相 | CIELAB、OKLab 与 W3C RGB | [A](op_specs/NBL-01A_hue_lab.md), [B](op_specs/NBL-01B_hue_oklab.md), [C](op_specs/NBL-01C_hue_w3c.md) |
| NBL-02 色度／饱和度 | 原生模型三种无彩色策略及 W3C 饱和度 | [A](op_specs/NBL-02A_chroma_lab_zero_hue.md), [B](op_specs/NBL-02B_chroma_oklab_zero_hue.md), [C](op_specs/NBL-02C_saturation_w3c.md), [D](op_specs/NBL-02D_chroma_lab_preserve_gray.md), [E](op_specs/NBL-02E_chroma_oklab_preserve_gray.md), [F](op_specs/NBL-02F_chroma_lab_source_hue.md), [G](op_specs/NBL-02G_chroma_oklab_source_hue.md) |
| NBL-03 颜色 | 源颜色属性与背景明度 | [A](op_specs/NBL-03A_color_lab.md), [B](op_specs/NBL-03B_color_oklab.md), [C](op_specs/NBL-03C_color_w3c.md) |
| NBL-04 明度 | 源明度与背景颜色属性 | [A](op_specs/NBL-04A_lightness_lab.md), [B](op_specs/NBL-04B_lightness_oklab.md), [C](op_specs/NBL-04C_luminosity_w3c.md) |

## 逐成员入口

共 77 个 Proposed 成员。

| 成员 | 类别／执行／成熟度 | 提议注册 key | 实现状态 |
| --- | --- | --- | --- |
| [BLN-01A Normal](op_specs/BLN-01A_normal.md) | primitive / tile / D1_draft | `blend.normal` | implemented |
| [BLN-02A Add](op_specs/BLN-02A_add.md) | primitive / tile / D1_draft | `blend.add` | implemented |
| [BLN-02B Subtract](op_specs/BLN-02B_subtract.md) | primitive / tile / D1_draft | `blend.subtract` | implemented |
| [BLN-02C Divide](op_specs/BLN-02C_divide.md) | primitive / tile / D1_draft | `blend.divide` | implemented |
| [BLN-03A Multiply](op_specs/BLN-03A_multiply.md) | primitive / tile / D1_draft | `blend.multiply` | implemented |
| [BLN-03B Multiply unbounded](op_specs/BLN-03B_multiply_unbounded.md) | primitive / tile / D1_draft | `blend.multiply_unbounded` | implemented |
| [BLN-04A Darken](op_specs/BLN-04A_darken.md) | primitive / tile / D1_draft | `blend.darken` | implemented |
| [BLN-04B Lighten](op_specs/BLN-04B_lighten.md) | primitive / tile / D1_draft | `blend.lighten` | implemented |
| [BLN-05A Screen](op_specs/BLN-05A_screen.md) | primitive / tile / D1_draft | `blend.screen` | implemented |
| [BLN-05B Screen unbounded](op_specs/BLN-05B_screen_unbounded.md) | primitive / tile / D1_draft | `blend.screen_unbounded` | implemented |
| [BLN-06A Overlay](op_specs/BLN-06A_overlay.md) | primitive / tile / D1_draft | `blend.overlay` | implemented |
| [BLN-07A Hard light](op_specs/BLN-07A_hard_light.md) | primitive / tile / D1_draft | `blend.hard_light` | not_implemented |
| [BLN-08A Soft light w3c](op_specs/BLN-08A_soft_light_w3c.md) | primitive / tile / D1_draft | `blend.soft_light_w3c` | not_implemented |
| [BLN-09A Color dodge](op_specs/BLN-09A_color_dodge.md) | primitive / tile / D1_draft | `blend.color_dodge` | not_implemented |
| [BLN-10A Color burn](op_specs/BLN-10A_color_burn.md) | primitive / tile / D1_draft | `blend.color_burn` | not_implemented |
| [BLN-11A Linear dodge](op_specs/BLN-11A_linear_dodge.md) | primitive / tile / D1_draft | `blend.linear_dodge` | not_implemented |
| [BLN-11B Linear burn](op_specs/BLN-11B_linear_burn.md) | primitive / tile / D1_draft | `blend.linear_burn` | not_implemented |
| [BLN-12A Linear light](op_specs/BLN-12A_linear_light.md) | primitive / tile / D1_draft | `blend.linear_light` | not_implemented |
| [BLN-13A Vivid light](op_specs/BLN-13A_vivid_light.md) | primitive / tile / D1_draft | `blend.vivid_light` | not_implemented |
| [BLN-14A Pin light](op_specs/BLN-14A_pin_light.md) | primitive / tile / D1_draft | `blend.pin_light` | not_implemented |
| [BLN-15A Difference](op_specs/BLN-15A_difference.md) | primitive / tile / D1_draft | `blend.difference` | not_implemented |
| [BLN-15B Exclusion](op_specs/BLN-15B_exclusion.md) | primitive / tile / D1_draft | `blend.exclusion` | not_implemented |
| [BLN-16A Hard mix vivid](op_specs/BLN-16A_hard_mix_vivid.md) | primitive / tile / D1_draft | `blend.hard_mix_vivid` | not_implemented |
| [BLN-16B Hard mix sum](op_specs/BLN-16B_hard_mix_sum.md) | primitive / tile / D1_draft | `blend.hard_mix_sum` | not_implemented |
| [BLN-17A Darker whole color by linear_y](op_specs/BLN-17A_darker_color_linear_y.md) | primitive / tile / D1_draft | `blend.darker_color_linear_y` | not_implemented |
| [BLN-17B Lighter whole color by linear_y](op_specs/BLN-17B_lighter_color_linear_y.md) | primitive / tile / D1_draft | `blend.lighter_color_linear_y` | not_implemented |
| [BLN-17C Darker whole color by oklab_l](op_specs/BLN-17C_darker_color_oklab_l.md) | primitive / tile / D1_draft | `blend.darker_color_oklab_l` | not_implemented |
| [BLN-17D Lighter whole color by oklab_l](op_specs/BLN-17D_lighter_color_oklab_l.md) | primitive / tile / D1_draft | `blend.lighter_color_oklab_l` | not_implemented |
| [BLN-17E Darker whole color by cielab_l](op_specs/BLN-17E_darker_color_cielab_l.md) | primitive / tile / D1_draft | `blend.darker_color_cielab_l` | not_implemented |
| [BLN-17F Lighter whole color by cielab_l](op_specs/BLN-17F_lighter_color_cielab_l.md) | primitive / tile / D1_draft | `blend.lighter_color_cielab_l` | not_implemented |
| [BLN-18A Seeded coverage dissolve](op_specs/BLN-18A_dissolve_counter64.md) | primitive / tile / D1_draft | `blend.dissolve_counter64` | not_implemented |
| [BLN-19A Add a linear color signal with retained alpha](op_specs/BLN-19A_emission_add_straight.md) | primitive / tile / D1_draft | `blend.emission_add_straight` | not_implemented |
| [BLN-20A Coverage-weighted image interpolation in linear coordinates](op_specs/BLN-20A_premul_mix_straight_linear.md) | primitive / tile / D1_draft | `blend.premul_mix_straight_linear` | not_implemented |
| [BLN-20B Coverage-weighted image interpolation in encoded coordinates](op_specs/BLN-20B_premul_mix_straight_encoded.md) | primitive / tile / D1_draft | `blend.premul_mix_straight_encoded` | not_implemented |
| [CMP-01A Clear an image](op_specs/CMP-01A_clear.md) | primitive / tile / D1_draft | `composite.clear` | not_implemented |
| [CMP-02A Select source](op_specs/CMP-02A_source.md) | primitive / tile / D1_draft | `composite.source` | not_implemented |
| [CMP-02B Select destination](op_specs/CMP-02B_destination.md) | primitive / tile / D1_draft | `composite.destination` | not_implemented |
| [CMP-03A Source over in linear RGB/Gray](op_specs/CMP-03A_source_over_linear.md) | primitive / tile / D1_draft | `composite.source_over_linear` | not_implemented |
| [CMP-03B Destination over in linear RGB/Gray](op_specs/CMP-03B_destination_over_linear.md) | primitive / tile / D1_draft | `composite.destination_over_linear` | not_implemented |
| [CMP-03C Source over with supplied blended color in linear coordinates](op_specs/CMP-03C_source_over_blended_color_linear.md) | primitive / tile / D1_draft | `composite.source_over_blended_color_linear` | not_implemented |
| [CMP-03D Source over in encoded RGB/Gray](op_specs/CMP-03D_source_over_encoded.md) | primitive / tile / D1_draft | `composite.source_over_encoded` | not_implemented |
| [CMP-03E Destination over in encoded RGB/Gray](op_specs/CMP-03E_destination_over_encoded.md) | primitive / tile / D1_draft | `composite.destination_over_encoded` | not_implemented |
| [CMP-03F Source over with supplied blended color in encoded coordinates](op_specs/CMP-03F_source_over_blended_color_encoded.md) | primitive / tile / D1_draft | `composite.source_over_blended_color_encoded` | not_implemented |
| [CMP-04A Source in in linear RGB/Gray](op_specs/CMP-04A_source_in_linear.md) | primitive / tile / D1_draft | `composite.source_in_linear` | not_implemented |
| [CMP-04B Destination in in linear RGB/Gray](op_specs/CMP-04B_destination_in_linear.md) | primitive / tile / D1_draft | `composite.destination_in_linear` | not_implemented |
| [CMP-04C Source in in encoded RGB/Gray](op_specs/CMP-04C_source_in_encoded.md) | primitive / tile / D1_draft | `composite.source_in_encoded` | not_implemented |
| [CMP-04D Destination in in encoded RGB/Gray](op_specs/CMP-04D_destination_in_encoded.md) | primitive / tile / D1_draft | `composite.destination_in_encoded` | not_implemented |
| [CMP-05A Source out in linear RGB/Gray](op_specs/CMP-05A_source_out_linear.md) | primitive / tile / D1_draft | `composite.source_out_linear` | not_implemented |
| [CMP-05B Destination out in linear RGB/Gray](op_specs/CMP-05B_destination_out_linear.md) | primitive / tile / D1_draft | `composite.destination_out_linear` | not_implemented |
| [CMP-05C Source out in encoded RGB/Gray](op_specs/CMP-05C_source_out_encoded.md) | primitive / tile / D1_draft | `composite.source_out_encoded` | not_implemented |
| [CMP-05D Destination out in encoded RGB/Gray](op_specs/CMP-05D_destination_out_encoded.md) | primitive / tile / D1_draft | `composite.destination_out_encoded` | not_implemented |
| [CMP-06A Source atop in linear RGB/Gray](op_specs/CMP-06A_source_atop_linear.md) | primitive / tile / D1_draft | `composite.source_atop_linear` | not_implemented |
| [CMP-06B Destination atop in linear RGB/Gray](op_specs/CMP-06B_destination_atop_linear.md) | primitive / tile / D1_draft | `composite.destination_atop_linear` | not_implemented |
| [CMP-06C Source atop in encoded RGB/Gray](op_specs/CMP-06C_source_atop_encoded.md) | primitive / tile / D1_draft | `composite.source_atop_encoded` | not_implemented |
| [CMP-06D Destination atop in encoded RGB/Gray](op_specs/CMP-06D_destination_atop_encoded.md) | primitive / tile / D1_draft | `composite.destination_atop_encoded` | not_implemented |
| [CMP-07A Xor in linear RGB/Gray](op_specs/CMP-07A_xor_linear.md) | primitive / tile / D1_draft | `composite.xor_linear` | not_implemented |
| [CMP-07B Xor in encoded RGB/Gray](op_specs/CMP-07B_xor_encoded.md) | primitive / tile / D1_draft | `composite.xor_encoded` | not_implemented |
| [CMP-08A Plus raw in linear RGB/Gray](op_specs/CMP-08A_plus_raw_linear.md) | primitive / tile / D1_draft | `composite.plus_raw_linear` | not_implemented |
| [CMP-08B Plus lighter bounded in linear RGB/Gray](op_specs/CMP-08B_plus_lighter_bounded_linear.md) | primitive / tile / D1_draft | `composite.plus_lighter_bounded_linear` | not_implemented |
| [CMP-08C Plus raw in encoded RGB/Gray](op_specs/CMP-08C_plus_raw_encoded.md) | primitive / tile / D1_draft | `composite.plus_raw_encoded` | not_implemented |
| [CMP-08D Plus lighter bounded in encoded RGB/Gray](op_specs/CMP-08D_plus_lighter_bounded_encoded.md) | primitive / tile / D1_draft | `composite.plus_lighter_bounded_encoded` | not_implemented |
| [NBL-01A Hue blend in lab](op_specs/NBL-01A_hue_lab.md) | primitive / tile / D1_draft | `blend.hue_lab` | not_implemented |
| [NBL-01B Hue blend in oklab](op_specs/NBL-01B_hue_oklab.md) | primitive / tile / D1_draft | `blend.hue_oklab` | not_implemented |
| [NBL-01C Hue blend in w3c](op_specs/NBL-01C_hue_w3c.md) | primitive / tile / D1_draft | `blend.hue_w3c` | not_implemented |
| [NBL-02A Chroma blend in lab](op_specs/NBL-02A_chroma_lab_zero_hue.md) | primitive / tile / D1_draft | `blend.chroma_lab_zero_hue` | not_implemented |
| [NBL-02B Chroma blend in oklab](op_specs/NBL-02B_chroma_oklab_zero_hue.md) | primitive / tile / D1_draft | `blend.chroma_oklab_zero_hue` | not_implemented |
| [NBL-02C Chroma blend in w3c](op_specs/NBL-02C_saturation_w3c.md) | primitive / tile / D1_draft | `blend.saturation_w3c` | not_implemented |
| [NBL-02D Chroma blend with preserve_gray in lab](op_specs/NBL-02D_chroma_lab_preserve_gray.md) | primitive / tile / D1_draft | `blend.chroma_lab_preserve_gray` | not_implemented |
| [NBL-02E Chroma blend with preserve_gray in oklab](op_specs/NBL-02E_chroma_oklab_preserve_gray.md) | primitive / tile / D1_draft | `blend.chroma_oklab_preserve_gray` | not_implemented |
| [NBL-02F Chroma blend with source_hue in lab](op_specs/NBL-02F_chroma_lab_source_hue.md) | primitive / tile / D1_draft | `blend.chroma_lab_source_hue` | not_implemented |
| [NBL-02G Chroma blend with source_hue in oklab](op_specs/NBL-02G_chroma_oklab_source_hue.md) | primitive / tile / D1_draft | `blend.chroma_oklab_source_hue` | not_implemented |
| [NBL-03A Color blend in lab](op_specs/NBL-03A_color_lab.md) | primitive / tile / D1_draft | `blend.color_lab` | not_implemented |
| [NBL-03B Color blend in oklab](op_specs/NBL-03B_color_oklab.md) | primitive / tile / D1_draft | `blend.color_oklab` | not_implemented |
| [NBL-03C Color blend in w3c](op_specs/NBL-03C_color_w3c.md) | primitive / tile / D1_draft | `blend.color_w3c` | not_implemented |
| [NBL-04A Lightness blend in lab](op_specs/NBL-04A_lightness_lab.md) | primitive / tile / D1_draft | `blend.lightness_lab` | not_implemented |
| [NBL-04B Lightness blend in oklab](op_specs/NBL-04B_lightness_oklab.md) | primitive / tile / D1_draft | `blend.lightness_oklab` | not_implemented |
| [NBL-04C Lightness blend in w3c](op_specs/NBL-04C_luminosity_w3c.md) | primitive / tile / D1_draft | `blend.luminosity_w3c` | not_implemented |

## 相关入口

- [共通规格](op_specs/COM_common_contract.md)
- [研究来源](research-sources.md)
- [严格舍入 oracle](../../../oracle/ops/09-composite/README.md)
- [抠像与 matte 基础算子](keying.md)
- [现有 Layer 值类型契约](../../kernel-architecture/Layer-Runtime.md)
