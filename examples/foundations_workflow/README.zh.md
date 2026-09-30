# Foundations 工作流示例

此 C++17 示例使用 `Photospider::kernel`，运行维护中的 `numeric` 与 `expression-lut` 场景。默认选择器 `all` 会运行这两个场景。程序仍接受 `generator-gain`，但 planar storage gate 会拒绝该选择器导入 typed-image snapshot，因此它不是当前可运行场景，也不属于 `all` 和 CTest。

| 场景 | 独立核验结果 |
| --- | --- |
| `numeric` | `[3,2,1] - [4,4,4] = [-1,-2,-3]`；`[1,2,3]` 的均值为 `2`，方差为 `2/3`。 |
| `expression-lut` | 采样 `x^2` 得到 `[0,0.25,1]`；线性 LUT 在 `0.25` 处得到 `0.125`。 |

在仓库根目录构建并运行示例：

```sh
cmake --build build --target photospider_foundations_workflow -j 8
build/examples/foundations_workflow/photospider_foundations_workflow --scenario all
ctest --test-dir build -R '^test_workflow_(numeric_reductions|expression_lut)$' --output-on-failure
```

可传入 `--scenario numeric` 或 `--scenario expression-lut` 单独运行一个维护中的场景。两个场景均通过后，程序打印 `Foundations scenarios=2 oracle=passed backend=cpu`。发生错误时打印 `Foundations failed: ...` 并以状态码 1 退出。

独立 CMake 项目要求已安装 Photospider 0.30。配置示例：

```sh
cmake -S examples/foundations_workflow -B build/foundations-consumer -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build/foundations-consumer -j 8
build/foundations-consumer/photospider_foundations_workflow --scenario all
```

此 consumer 只链接 `Photospider::kernel`。源码中的结果检查和两个 CTest 用例覆盖上述 CPU 场景，不验证其他算子族或原生 GPU 执行。

[英文说明](README.md)为权威来源。
