# 多输出切分示例

此 C++17 源码使用公开命名输出与可选 joint 执行 API。保留的
`image.split_horizontal` 场景分别请求 `full`、`left`、`right`，检查源坐标偏移。
旧滤镜场景已随内建实现移除。

```sh
cmake --build build --target photospider_multi_output_workflow -j 8
build/examples/multi_output_workflow/photospider_multi_output_workflow --joint on
```

旧 typed 图像绑定仍需迁移到 planar，当前示例不作为运行时验收。
活动验证由 `test_multi_output_execution` 和 planar workflow 测试承担。
独立安装消费可使用
`cmake -S examples/multi_output_workflow -B build/multi-output-consumer -DCMAKE_PREFIX_PATH=/path/to/install`。
详见[多输出契约](../../docs/kernel-architecture/zh/Multi-Output-Operations.zh.md)。
[英文说明](README.md)为权威来源。
