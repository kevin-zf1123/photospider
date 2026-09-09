# 候选依赖与实现方式选择

状态Proposed。本页记录研究日2026-09-09的候选组件事实和集成职责，不表示项目已安装、接受或验证这些依赖。具体平台、性能、附加codec/model许可与安装消费需要在相关实现切片中确认。

| 组件 | 已核验版本 / 许可标识 | 可承接职责 | 必须适配的边界 |
| --- | --- | --- | --- |
| LibRaw | 0.22.2，2026-07-16；LGPL-2.1或CDDL-1.0 | RAW decode/metadata | 项目明确不以生产级完整RAW渲染为目标；已扣黑/WB状态、相机支持子集[^raw] |
| OpenImageIO | stable3.1.17.0，2026-09-01；Apache-2.0，3.2为beta | codec、metadata、多通道、部分算法/统计 | 稳定/开发文档区分；线程、内存、FFT norm与像素语义[^oiio] |
| OpenEXR | 3.4.15，2026-08-21；BSD-3-Clause | HDR/multipart/deep文件 | 保存格式与Deep算法不同，data window/ID/emission[^exr] |
| OpenColorIO | 2.5.2，2026-05-13；BSD-3-Clause | 具名颜色、look、display processor | config/context/LUT资源快照，default interpolation不可当固定语义[^ocio] |
| ONNX Runtime | 1.29.0，2026-08-12；MIT | 模型推理候选 | provider×op×precision矩阵、模型独立许可、pre/post与tile[^ort] |
| Little CMS | 本次阅读API2.18；未锁定集成包/许可审查 | CPU ICC transform/proof/gamut | intent/proofing intent、BPC、线程与buffer；单独评估 |
| FFT实现 | 阅读FFTW3.3.11与SciPy语义；未选定生产库 | DFT/rFFT、固定核FFT、波动PSF | license/build、norm、real layout、规划临时内存、host线程/取消 |
| OpenCV | 阅读4.13.0与部分5.0官方API；未锁版本 | 几何、滤波、配准、光流、修复候选 | correlation方向、中心坐标、边界、dtype、内部线程和内存 |

这些库的职责可并存：OIIO读取图像不替代OCIO/ICC语义选择，LibRaw解码不替代camera profile与创意显影，OpenEXR读取Deep不替代Deep合成，模型runtime不替代模型质量。少量逐元素和小核算子可直接实现，避免为简单公式引入大依赖。

## 算法到后端

| 类型 | CPU参考方式 | 优化候选 | 选择依据 |
| --- | --- | --- | --- |
| E逐元素/小矩阵 | 明确Float32阶段或Float64参考 | SIMD/Metal | 转换/dispatch开销与数值范围 |
| H局部核 | 直接非对称核oracle | separable、滑窗、SIMD、GPUtile | 核结构、半径、ROI、临时空间 |
| W全局频域 | 配对DFT norm和padding | FFT、overlap-add/save | 不把大核自动全部送FFT |
| S scan/reduce | 固定顺序/稳定累加 | 并行prefix、分块确定性归约 | 跨tile状态、精度与allocation |
| R任意采样 | Float64 map+参考sampler | 合成map、mip/EWA、GPUtexture | aliasing/负瓣/invalid map、保守demand |
| 迭代/图算法 | 具名solver与收敛 | 预条件、并行solver | 残差、全局边界、取消粒度 |
| ML | 固定模型与pre/post reference | 受支持execution provider | 同一模型/数据域的误差、内存、吞吐 |

近似快速模式应显式命名并给质量参数，CPUExact/MetalFp32现有含义不能自动覆盖新算法。不要为每个op创建自己的线程池、GPU资源系统或文件缓存。宿主/内核继续拥有其已接受的资源和生命周期。

## 来源

[^raw]: LibRaw，[0.22.2 release](https://github.com/LibRaw/LibRaw/releases/tag/0.22.2)、[项目范围与许可](https://www.libraw.org/)。
[^oiio]: ASWF，[OpenImageIO releases](https://github.com/AcademySoftwareFoundation/OpenImageIO/releases)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenImageIO/main/LICENSE.md)。
[^exr]: ASWF，[OpenEXR release news](https://openexr.com/en/latest/)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/openexr/main/LICENSE.md)。
[^ocio]: ASWF，[OpenColorIO releases](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/main/LICENSE)。
[^ort]: Microsoft，[ONNX Runtime1.29.0](https://github.com/microsoft/onnxruntime/releases/tag/v1.29.0)、[LICENSE](https://raw.githubusercontent.com/microsoft/onnxruntime/main/LICENSE)、[execution providers](https://onnxruntime.ai/docs/execution-providers/)。

其他接口阅读来源：[Little CMS2.18 API](https://www.littlecms.com/LittleCMS2.18%20API.pdf)、[FFTW计算定义](https://www.fftw.org/fftw3_doc/What-FFTW-Really-Computes.html)、[OpenCV4.13 Filtering](https://docs.opencv.org/4.13.0/d4/d86/group__imgproc__filter.html)。未在本页选定的许可/版本不得在后续实现中写成已经完成依赖评审。
