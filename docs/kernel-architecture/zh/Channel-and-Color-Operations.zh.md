# 通道与颜色算子

包 0.20.0 移除旧通道、alpha、颜色算子，以及 `numeric.cast`、
`numeric.encode_range` 的源码、默认注册和专属旧测试。13 个完整 key 与保留范围见
[退休记录](../../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。
查询、直接调用与编译这些 key 均返回 `NotFound`，无别名或兼容占位注册。

[目录](../../built-in_ops/02-format-color/representation.md)及
[FMT 公共规格](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
定义后续方向。FMT-01..08 保持 Proposed/未实现，FMT-07 已退休。完整图像使用
planar 存储、straight 颜色和同张量内的 alpha。新算子须实现各自的精确请求、
metadata、数值和布局契约；旧 typed HWC 行为不构成新规格的子集实现。

NUM/CRV 使用的共享 ColorArray 描述、profile 所有权和数学基础设施保留。
这些能力不代表已注册格式转换，也不代表实现了新 FMT 算子族。

[公开退休回归](../../../tests/integration/test_format_color_retirement.cpp)覆盖
全部旧 key 的查询、直接调用和编译，并运行独立核验结果为 0.5 的
`numeric.add_strict` workflow。安装消费测试编译运行同一源码：

```sh
cmake --build build --target test_format_color_retirement -j 8
ctest --test-dir build -R '^(test_format_color_retirement|test_installed_consumer)$' --output-on-failure
```

[英文说明](../Channel-and-Color-Operations.md)为权威来源。
