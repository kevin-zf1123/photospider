# Foundations workflow

本 C++17 示例使用公开 WorkflowDocument、Compiler 和 ExecutionContext，
消费 0.27 安装包。默认 `all` 运行两个维护中的 generic 场景；格式和颜色算子族
使用各自的集成测试与示例。

| 场景 | 独立核验的结果 |
| --- | --- |
| `numeric` | `[3,2,1]-[4,4,4]=[-1,-2,-3]`；`[1,2,3]` 的 mean=2、variance=2/3 |
| `expression-lut` | 平方采样 `[0,.25,1]`；线性 LUT 在 .25 得到 .125 |

```sh
cmake --build build --target photospider_foundations_workflow -j 8
build/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build -R '^test_workflow_(numeric_reductions|expression_lut)$' --output-on-failure
```

成功结束时打印 `Foundations scenarios=2 oracle=passed backend=cpu`。
可分别指定 `--scenario numeric`或 `expression-lut`。

消费已有 0.27 安装包时，使用
`cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install`
配置，再构建该目录；仅链接 `Photospider::kernel`。

显式 `generator-gain` 选择器保留为旧 typed image
迁移源码，已排除在 `all` 和活动验收集之外，其旧路径不代表 planar 支持。
完整删除范围见[退休记录](../../docs/built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。
此示例不提供替代格式转换。[英文说明](README.md)为权威来源。
