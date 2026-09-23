# Foundations workflow

本 C++17 示例使用公开 WorkflowDocument、Compiler 和 ExecutionContext。包 0.20.0
删除旧 cast/range、channels、alpha/color、image-curve 和依赖通道转换的 filter
场景。新 FMT 规格尚未实现。默认 `all` 运行三个维护中的 generic 场景：

| 场景 | 独立核验的结果 |
| --- | --- |
| `numeric` | `[3,2,1]-[4,4,4]=[-1,-2,-3]`；`[1,2,3]` 的 mean=2、variance=2/3 |
| `expression-lut` | 平方采样 `[0,.25,1]`；线性 LUT 在 .25 得到 .125 |
| `basic-filters` | 非对称卷积／相关与误差直方图符合 fixture oracle |

```sh
cmake --build build --target photospider_foundations_workflow -j 8
build/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build -R '^test_foundations_(numeric|expression-lut|basic-filters)$' --output-on-failure
```

成功结束时打印 `Foundations scenarios=3 oracle=passed backend=cpu`。
可分别指定 `--scenario numeric`、`expression-lut` 或 `basic-filters`。

消费已有 0.20 安装包时，使用
`cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install`
配置，再构建该目录；仅链接 `Photospider::kernel`。

显式 `generator-gain`、`components`、`basic-masks` 选择器保留为旧 typed image/mask
迁移源码，已排除在 `all` 和活动验收集之外，其旧路径不代表 planar 支持。
完整删除范围见[退休记录](../../docs/built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。
此示例不提供替代格式转换。[英文说明](README.md)为权威来源。
