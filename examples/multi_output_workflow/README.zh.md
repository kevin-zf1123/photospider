# 多输出图像迁移示例

本 C++17 源码使用公开命名输出和可选 joint 执行 API。包 0.20.0 移除 `420` 场景及其
`color.rgb_to_ycbcr420` 依赖。保留 `split`、`channels`、`gaussian` 三个旧图像场景
供迁移。它们能够构建，但旧 typed 图像绑定会被当前 planar 存储门禁拒绝，
不属于活动运行验收。

```sh
cmake --build build --target photospider_multi_output_workflow -j 8
build/examples/multi_output_workflow/photospider_multi_output_workflow --help
```

源码保留独立偏移、卷积与高斯系数 oracle。Gaussian 复算使用单独提供的 R 参考平面，
不再调用已退休的通道提取。`all|split|channels|gaussian`、`--joint on|off`、
`--radius`、`--sigma` 参数保留供迁移使用；成功执行图像仍需完成迁移。

独立构建消费已安装的 Photospider 0.20：
`cmake -S examples/multi_output_workflow -B build/multi-output-consumer -DCMAKE_PREFIX_PATH=/path/to/install`。
安装消费测试构建此示例，但不将它列作通过的图像运行测试，另行运行
[格式退休回归](../../tests/integration/test_format_color_retirement.cpp)。

支持边界见[多输出说明](../../docs/kernel-architecture/zh/Multi-Output-Operations.zh.md)，
删除范围见[FMT 退休记录](../../docs/built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。
[英文说明](README.md)为权威来源。
