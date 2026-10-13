# Foundations 工作流示例

| 场景 | 独立核验结果 |
| --- | --- |
| `numeric` | `[3,2,1] - [4,4,4] = [-1,-2,-3]`；`[1,2,3]` 的均值为 `2`，方差为 `2/3`。 |
| `expression-lut` | 采样 `x^2` 得到 `[0,0.25,1]`；线性 LUT 在 `0.25` 处得到 `0.125`。 |

在仓库根目录构建并运行：

独立 CMake 项目要求已安装 Photospider 0.30。配置示例：

```sh
cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/foundations-consumer -j8
build/foundations-consumer/photospider_foundations_workflow --scenario all
```

[英文说明](README.md)为权威来源。
