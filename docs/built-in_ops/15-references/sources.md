# 来源索引与证据边界

统一访问日为2026-09-09。分类文档的脚注保留标题、作者/发布方、版本或日期和支持主张；本页汇总全部唯一外部URL，并链接到采用它的分类文档。滚动页面的版本不等于本项目已安装版本。以下资料来自官方指南、标准、原始论文或原作者实现，许可证和release页面仅支持其对应的元数据事实。

## 证据等级

- 规范/原论文：支持具名数学定义、算法与约束；算法选择和Photospider默认值仍为Proposed。
- 官方产品指南：支持功能存在和明确公开行为，不能推导未公开核系数、alpha分支或逐像素兼容。
- 代码阅读：本地现有8项算子与相邻散景源码事实；本次未重新执行这些产品测试。
- 数学推导：LUT离散查表、位移合成、圆弧bbox、PSD归一等解析检查；不冒充GPU或商业软件实测。

## 私有材料与读取限制

1. 引用对话《优化逐像素卷积》，conversationId `6a7ca248-cfb4-83ea-8771-725a9a4d6dff`：已读取，作为问题/候选推导，不将对话内不可解析的引用ID作为文献。
2. 引用对话《光栅化映射证明》，conversationId `6a2cc4fb-f964-83ee-8ee8-9b3df3b0ec68`：已读取，圆弧、退化、bbox和chart限制在投影篇明确。
3. 本地 `photospider` 工作区 HEAD `18d2126`：类型、ABI、注册与Region源码；详见当前实现页。本次只增加本目录文档。
4. 本地 `/Users/zhufeng/document/code/phisical_bokeh`：固定PSF实现与规格；源码位置和本次未运行边界在散景篇列出。
5. ShaderToy `wcGBDD`、`s3XGz4`：实际尝试读取，未取得源码。下方URL仅登记待核验参考，不作为算法证据。
6. Kosloff GI2009 PDF：官方索引正文可取得，完整PDF读取失败；只采用已读段落与独立数学推导。旧Resolve20 Colorist Guide和Adobe PDF1.7的部分索引材料用于研究线索，最终本文不以未通读部分声明完整产品兼容。

关键未决项包括CSP Glow公式、现代PS/CSP非可分离模式的profile路径、ShaderToy源码及许可、真实镜头校准与性能、模型/相机完整支持列表。没有将这些未知填写成确定实现。

## 外部文献目录

下面每项的“用于”指向主张上下文。相同来源在多篇使用时合并URL；近邻脚注的完整书目优先于短链接标题。


1. John K. Salmon 等，*Parallel Random Numbers: As Easy as 1, 2, 3*，SC11，2011；[作者与项目页](https://random123.com/)。支持 counter-based 并行随机数设计，具体 Photospider 键布局和质量模式仍为建议。

   用于：[00-foundation/contracts.md](../00-foundation/contracts.md)。

2. LibRaw，[0.22.2 release](https://github.com/LibRaw/LibRaw/releases/tag/0.22.2)、[项目范围与许可](https://www.libraw.org/)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

3. LibRaw，[0.22.2 release](https://github.com/LibRaw/LibRaw/releases/tag/0.22.2)、[项目范围与许可](https://www.libraw.org/)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

4. ASWF，[OpenImageIO releases](https://github.com/AcademySoftwareFoundation/OpenImageIO/releases)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenImageIO/main/LICENSE.md)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

5. ASWF，[OpenImageIO releases](https://github.com/AcademySoftwareFoundation/OpenImageIO/releases)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenImageIO/main/LICENSE.md)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

6. ASWF，[OpenEXR release news](https://openexr.com/en/latest/)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/openexr/main/LICENSE.md)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

7. ASWF，[OpenEXR release news](https://openexr.com/en/latest/)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/openexr/main/LICENSE.md)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

8. ASWF，[OpenColorIO releases](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/main/LICENSE)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

9. ASWF，[OpenColorIO releases](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases)、[LICENSE](https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/main/LICENSE)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

10. Microsoft，[ONNX Runtime1.29.0](https://github.com/microsoft/onnxruntime/releases/tag/v1.29.0)、[LICENSE](https://raw.githubusercontent.com/microsoft/onnxruntime/main/LICENSE)、[execution providers](https://onnxruntime.ai/docs/execution-providers/)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

11. Microsoft，[ONNX Runtime1.29.0](https://github.com/microsoft/onnxruntime/releases/tag/v1.29.0)、[LICENSE](https://raw.githubusercontent.com/microsoft/onnxruntime/main/LICENSE)、[execution providers](https://onnxruntime.ai/docs/execution-providers/)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

12. Microsoft，[ONNX Runtime1.29.0](https://github.com/microsoft/onnxruntime/releases/tag/v1.29.0)、[LICENSE](https://raw.githubusercontent.com/microsoft/onnxruntime/main/LICENSE)、[execution providers](https://onnxruntime.ai/docs/execution-providers/)。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

13. [Little CMS2.18 API](https://www.littlecms.com/LittleCMS2.18%20API.pdf)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)、[02-format-color/representation.md](../02-format-color/representation.md)。

14. [FFTW计算定义](https://www.fftw.org/fftw3_doc/What-FFTW-Really-Computes.html)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)。

15. OpenCV，[*Image Filtering*](https://docs.opencv.org/4.13.0/d4/d86/group__imgproc__filter.html)，4.13.0；correlation、核和边界接口。

   用于：[00-foundation/dependencies.md](../00-foundation/dependencies.md)、[05-filter/spatial.md](../05-filter/spatial.md)。

16. Academy，[*CLF Specification*](https://docs.acescentral.com/clf/specification/)，CLF v3；[Implementation Guide](https://docs.acescentral.com/clf/guides/)，LUT形状/顺序和独立测试资料。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[01-numeric/curves.md](../01-numeric/curves.md)、[07-grade/adjustments.md](../07-grade/adjustments.md)。

17. International Color Consortium，[*ICC.1:2022 Profile Specification*](https://www.color.org/specification/ICC.1-2022-05.pdf)，2022-05；profile、PCS与rendering intent。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[02-format-color/representation.md](../02-format-color/representation.md)。

18. OpenEXR，[*Technical Introduction*](https://openexr.com/en/latest/TechnicalIntroduction.html)，Deep与数据角色。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

19. W3C，[*Compositing and Blending Level 1*](https://www.w3.org/TR/compositing-1/)，Candidate Recommendation Draft，2024-03-21；Porter-Duff及有界separable/nonseparable公式。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[09-composite/blending.md](../09-composite/blending.md)。

20. CELSYS，[*Blending modes*](https://help.clip-studio.com/en-us/manual_en/180_layers/Blending_modes.htm)，CLIP STUDIO PAINT在线手册；Glow模式名称与效果。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[09-composite/blending.md](../09-composite/blending.md)。

21. He、Sun、Tang，[*Guided Image Filtering*](https://people.csail.mit.edu/kaiming/publications/eccv10guidedfilter.pdf)，ECCV2010，公式5/6/8。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[05-filter/spatial.md](../05-filter/spatial.md)。

22. OpenCV，[*Geometric Image Transformations*](https://docs.opencv.org/4.13.0/da/d54/group__imgproc__transform.html)，4.13.0。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[08-transform/geometry.md](../08-transform/geometry.md)。

23. AMD GPUOpen，[*FidelityFX Depth of Field1.1*](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/)，访问2026-09-09。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[06-optics/bokeh.md](../06-optics/bokeh.md)。

24. Kannala、Brandt，[*A Generic Camera Model and Calibration Method for Conventional, Wide-Angle, and Fish-Eye Lenses*](https://users.aalto.fi/~kannalj1/calibration/Kannala_Brandt_calibration.pdf)，PAMI2006。

   用于：[00-foundation/research-report.md](../00-foundation/research-report.md)、[08-transform/projections.md](../08-transform/projections.md)。

25. Academy，[*CLF Specification*](https://docs.acescentral.com/clf/specification/)，CLF v3；[Implementation Guide](https://docs.acescentral.com/clf/guides/)，LUT形状/顺序和独立测试资料。

   用于：[01-numeric/curves.md](../01-numeric/curves.md)。

26. SciPy，[*PchipInterpolator*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)，访问页面v1.18.0；单调插值与节点约束。

   用于：[01-numeric/curves.md](../01-numeric/curves.md)。

27. SciPy，[*BSpline*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html)，访问页面v1.18.0；degree/knots。

   用于：[01-numeric/curves.md](../01-numeric/curves.md)。

28. SciPy，[*resample*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample.html)，访问页面v1.18.0；Fourier周期假设。

   用于：[01-numeric/curves.md](../01-numeric/curves.md)。

29. OpenImageIO，[*ImageBufAlgo: Image Processing*](https://openimageio.readthedocs.io/en/latest/imagebufalgo.html)，滚动官方文档，访问2026-09-09。仅用于API职责示例，不据此宣称稳定版号。

   用于：[02-format-color/representation.md](../02-format-color/representation.md)。

30. Academy Software Foundation，[*OpenColorIO Documentation*](https://opencolorio.readthedocs.io/en/latest/)，滚动文档；配置、processor与display/view体系。

   用于：[02-format-color/representation.md](../02-format-color/representation.md)。

31. W3C，[*CSS Color Module Level 4*](https://www.w3.org/TR/css-color-4/)，滚动规范草案；Lab、OKLab、色彩转换与色域概念。

   用于：[02-format-color/representation.md](../02-format-color/representation.md)。

32. ITU-R，[*BT.2100-3: Image parameter values for high dynamic range television*](https://www.itu.int/rec/R-REC-BT.2100)，2025；指定版本的PQ/HLG与量化要求。

   用于：[02-format-color/representation.md](../02-format-color/representation.md)。

33. OpenEXR，[*Technical Introduction: Premultiplied vs. Un-Premultiplied Color Channels*](https://openexr.com/en/latest/TechnicalIntroduction.html#premultiplied-vs-un-premultiplied-color-channels)，滚动官方文档；零alpha发光颜色。

   用于：[02-format-color/representation.md](../02-format-color/representation.md)。

34. W3C，[*SVG2 Paint Servers*](https://www.w3.org/TR/SVG2/pservers.html)，2018-10-04 CR；渐变几何与spread。

   用于：[03-generation/generators.md](../03-generation/generators.md)。

35. W3C，[*CSS Color4 Interpolation*](https://www.w3.org/TR/css-color-4/#interpolation)，访问2026-09-09；颜色/极坐标/alpha插值。

   用于：[03-generation/generators.md](../03-generation/generators.md)。

36. Ken Perlin，[*Improved Noise reference implementation*](https://mrl.cs.nyu.edu/~perlin/noise/)，2002。

   用于：[03-generation/generators.md](../03-generation/generators.md)。

37. D. E. Shaw Research，[*Random123*](https://github.com/DEShawResearch/random123)，SC11论文2011及官方实现。

   用于：[03-generation/generators.md](../03-generation/generators.md)。

38. Wolfe等，[*Spatiotemporal Blue Noise Masks*](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks)，NVIDIA Research，2022-07-06。

   用于：[03-generation/generators.md](../03-generation/generators.md)。

39. Adobe，[*Generate effects: Cell Pattern*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/generate-effects.html)，滚动英文指南。

   用于：[03-generation/generators.md](../03-generation/generators.md)。

40. W3C，[*SVG2 Paths*](https://www.w3.org/TR/SVG2/paths.html)，2018-10-04 CR；路径动作/子路径。

   用于：[03-generation/paths.md](../03-generation/paths.md)。

41. Loop、Blinn，[*Resolution Independent Curve Rendering using Programmable Graphics Hardware*](https://www.microsoft.com/en-us/research/publication/resolution-independent-curve-rendering-using-programmable-graphics-hardware/)，SIGGRAPH2005。

   用于：[03-generation/paths.md](../03-generation/paths.md)。

42. CELSYS，[*Vector layers*](https://help.clip-studio.com/en-us/manual_en/180_layers/Vector_layers.htm)，滚动手册；线宽编辑方式。

   用于：[03-generation/paths.md](../03-generation/paths.md)。

43. W3C，[*Compositing and Blending Level1*](https://www.w3.org/TR/compositing-1/#porterduffcompositingoperators)，2024CRD；coverage代数。

   用于：[04-mask-morphology/masks.md](../04-mask-morphology/masks.md)。

44. SciPy，[*grey_dilation*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.grey_dilation.html)，访问v1.18.0。

   用于：[04-mask-morphology/masks.md](../04-mask-morphology/masks.md)。

45. SciPy，[*maximum_filter1d*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.ndimage.maximum_filter1d.html)，访问v1.18.0。

   用于：[04-mask-morphology/masks.md](../04-mask-morphology/masks.md)。

46. Felzenszwalb、Huttenlocher，[*Distance Transforms of Sampled Functions*](https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf)，2012-09-02。

   用于：[04-mask-morphology/masks.md](../04-mask-morphology/masks.md)。

47. Cao等，[*Parallel Banding Algorithm*](https://www.comp.nus.edu.sg/~tants/pba.html)，I3D2010，作者页含2019更新。

   用于：[04-mask-morphology/masks.md](../04-mask-morphology/masks.md)。

48. Rong、Tan，[*Jump Flooding in GPU*](https://www.comp.nus.edu.sg/~tants/jfa.html)，I3D2006。

   用于：[04-mask-morphology/masks.md](../04-mask-morphology/masks.md)。

49. FFTW，[*DFT Definition*](https://www.fftw.org/fftw3_doc/The-1d-Discrete-Fourier-Transform-_0028DFT_0029.html)、[*Real-data Array Format*](https://www.fftw.org/fftw3_doc/Real_002ddata-DFT-Array-Format.html)，访问3.3.11。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

50. FFTW，[*DFT Definition*](https://www.fftw.org/fftw3_doc/The-1d-Discrete-Fourier-Transform-_0028DFT_0029.html)、[*Real-data Array Format*](https://www.fftw.org/fftw3_doc/Real_002ddata-DFT-Array-Format.html)，访问3.3.11。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

51. SciPy，[*fftconvolve*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.fftconvolve.html)、[*oaconvolve*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.oaconvolve.html)，滚动官方API。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

52. SciPy，[*fftconvolve*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.fftconvolve.html)、[*oaconvolve*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.oaconvolve.html)，滚动官方API。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

53. scikit-image，[*Restoration API*](https://scikit-image.org/docs/stable/api/skimage.restoration.html)，滚动文档；迭代/正则/clip行为，Photospider默认另定义。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

54. Buades、Coll、Morel，[*Non-Local Means Denoising*](https://www.ipol.im/pub/art/2011/bcm_nlm/)，IPOL，2011-09-13。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

55. Dabov等，[*BM3D author project*](https://webpages.tuni.fi/foi/GCF-BM3D/index.html)，原论文2007及后续变体。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

56. Getreuer，[*ROF Total Variation Denoising using Split Bregman*](https://www.ipol.im/pub/art/2012/g-tvd/)，IPOL2012。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

57. Mäkitalo、Foi，[*Anscombe/GAT inversion research*](https://webpages.tuni.fi/foi/invansc/index.html)，2011/2013。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

58. He、Sun、Tang，[*Single Image Haze Removal using Dark Channel Prior*](https://people.csail.mit.edu/kaiming/cvpr09/index.html)，2009/2011。

   用于：[05-filter/frequency-restoration.md](../05-filter/frequency-restoration.md)。

59. OpenCV，[*Feature Detection*](https://docs.opencv.org/5.0/main_modules/imgproc_feature.html)，访问5.0文档；Canny阈值/连接。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

60. Paris、Hasinoff、Kautz，[*Local Laplacian Filters*](https://people.csail.mit.edu/sparis/publi/2011/siggraph/)，SIGGRAPH2011。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

61. Pharr等，[*Image Reconstruction*](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Image_Reconstruction)，PBRT4，2023。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

62. Jimenez等，[*SMAA*](https://www.iryoku.com/smaa/)，Eurographics2012。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

63. Adobe，[*Filter effects reference*](https://helpx.adobe.com/uk/photoshop/using/filter-effects-reference.html)，2024-10-14；[ACR Sharpening](https://helpx.adobe.com/ca/camera-raw/desktop/using/sharpening-noise-reduction-camera-raw.html)，2023-11-03。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

64. Adobe，[*Filter effects reference*](https://helpx.adobe.com/uk/photoshop/using/filter-effects-reference.html)，2024-10-14；[ACR Sharpening](https://helpx.adobe.com/ca/camera-raw/desktop/using/sharpening-noise-reduction-camera-raw.html)，2023-11-03。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

65. CELSYS，[*Filters*](https://help.clip-studio.com/en-us/manual_en/390_filters/Filters.htm)，CSP英文手册；Smoothing等功能。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)、[09-composite/keying-paint.md](../09-composite/keying-paint.md)。

66. [Lightroom Enhance texture and details](https://helpx.adobe.com/lt/lightroom-cc/how-to/enhance-texture-details.html)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[05-filter/spatial.md](../05-filter/spatial.md)。

67. Foundry，[*ZDefocus*](https://learn.foundry.com/nuke/content/reference_guide/filter_nodes/zdefocus.html)，滚动参考指南。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

68. Kosloff、Tao、Barsky，[*Depth of Field Postprocessing for Layered Scenes Using Constant-Time Rectangle Spreading*](https://graphics.berkeley.edu/papers/Kosloff-DOF-2009-05/Kosloff-DOF-2009-05.pdf)，GI2009，§5；已核验官方索引正文，完整PDF读取失败。[作者博士论文](https://escholarship.org/uc/item/0161q94f)，2010补充PSF结构相关方法。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

69. Kosloff、Tao、Barsky，[*Depth of Field Postprocessing for Layered Scenes Using Constant-Time Rectangle Spreading*](https://graphics.berkeley.edu/papers/Kosloff-DOF-2009-05/Kosloff-DOF-2009-05.pdf)，GI2009，§5；已核验官方索引正文，完整PDF读取失败。[作者博士论文](https://escholarship.org/uc/item/0161q94f)，2010补充PSF结构相关方法。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

70. Garcia，[*Circular Separable Convolution Depth of Field*](https://media.gdcvault.com/gdc2018/presentations/Garcia_Kleber_CircularDepthOf.pdf)，EA/Frostbite，GDC2018。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

71. POPPY，[*Overview*](https://poppy-optics.readthedocs.io/en/latest/overview.html)、[*Extending POPPY*](https://poppy-optics.readthedocs.io/en/latest/extending.html)，滚动官方文档。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

72. POPPY，[*Overview*](https://poppy-optics.readthedocs.io/en/latest/overview.html)、[*Extending POPPY*](https://poppy-optics.readthedocs.io/en/latest/extending.html)，滚动官方文档。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

73. PBRT4，[*Color*](https://pbr-book.org/4ed/Radiometry%2C_Spectra%2C_and_Color/Color)，2023；光谱到颜色的多对一映射。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

74. Adobe，[*Blur and Sharpen Effects*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/blur-sharpen-effects.html)，Camera Lens Blur段。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

75. Ansys，[*OpticStudio Standard Spot Diagram*](https://ansyshelp.ansys.com/public/Views/Secured/Zemax/v25101/en/OpticStudio_User_Guide/OpticStudio_Help/topics/Standard_Spot_Diagram.html)，v25101；Airy半径与成像侧有效f-number。

   用于：[06-optics/bokeh.md](../06-optics/bokeh.md)。

76. Adobe，[*Levels adjustment*](https://helpx.adobe.com/photoshop/using/levels-adjustment.html)，滚动Photoshop指南；输入/输出色阶和中间调控制。

   用于：[07-grade/adjustments.md](../07-grade/adjustments.md)。

77. Adobe，[*Adjust shadow and highlight detail*](https://helpx.adobe.com/photoshop/using/adjust-shadow-highlight-detail.html)，滚动Photoshop指南；局部radius与tonal width。

   用于：[07-grade/adjustments.md](../07-grade/adjustments.md)。

78. Blackmagic Design，[*DaVinci Resolve Color*](https://www.blackmagicdesign.com/products/davinciresolve/color)，2026-09-09访问页面标示Resolve21；调色工具功能覆盖，未提供完整算法公式。

   用于：[07-grade/adjustments.md](../07-grade/adjustments.md)、[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

79. Pharr、Jakob、Humphreys，[*Image Texture*](https://www.pbr-book.org/4ed/Textures_and_Materials/Image_Texture)，PBRT4，2023。

   用于：[08-transform/geometry.md](../08-transform/geometry.md)。

80. Adobe，[*Overview of Liquify filter*](https://helpx.adobe.com/photoshop/desktop/effects-filters/artistic-stylize-filters/overview-of-liquify-filter.html)，2026-02-23。

   用于：[08-transform/geometry.md](../08-transform/geometry.md)。

81. Schaefer、McPhail、Warren，[*Image Deformation Using Moving Least Squares*](https://people.engr.tamu.edu/schaefer/research/mls.pdf)，SIGGRAPH2006。

   用于：[08-transform/geometry.md](../08-transform/geometry.md)。

82. Foundry，[*STMap*](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/stmap.html)，滚动参考；[SplineWarp](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/splinewarp.html)记录不同warp版本的影响范围差异。

   用于：[08-transform/geometry.md](../08-transform/geometry.md)。

83. Foundry，[*STMap*](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/stmap.html)，滚动参考；[SplineWarp](https://learn.foundry.com/nuke/content/reference_guide/transform_nodes/splinewarp.html)记录不同warp版本的影响范围差异。

   用于：[08-transform/geometry.md](../08-transform/geometry.md)。

84. [ShaderToy wcGBDD](https://www.shadertoy.com/view/wcGBDD)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[08-transform/projections.md](../08-transform/projections.md)。

85. [s3XGz4](https://www.shadertoy.com/view/s3XGz4)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[08-transform/projections.md](../08-transform/projections.md)。

86. OpenCV，[*Fisheye camera model*](https://docs.opencv.org/4.13.0/db/d58/group__calib3d__fisheye.html)，4.13.0。

   用于：[08-transform/projections.md](../08-transform/projections.md)。

87. Thomas Porter、Tom Duff，[*Compositing Digital Images*](https://keithp.com/~keithp/porterduff/p253-porter.pdf)，SIGGRAPH 1984，pp.253–259；原论文镜像，alpha合成模型。

   用于：[09-composite/blending.md](../09-composite/blending.md)。

88. Adobe，[*Blending modes*](https://helpx.adobe.com/photoshop/using/blending-modes.html)，滚动Photoshop用户指南；模式功能描述，未作为所有分支数值兼容证据。

   用于：[09-composite/blending.md](../09-composite/blending.md)。

89. Foundry，[*Merge*](https://learn.foundry.com/nuke/12.2/content/reference_guide/merge_nodes/merge.html)，Nuke12.2参考；HDR/negative/alpha开关和模式差异。

   用于：[09-composite/blending.md](../09-composite/blending.md)。

90. Foundry，[*IBKColor*](https://learn.foundry.com/nuke/content/reference_guide/keyer_nodes/ibkcolor.html)，滚动参考指南；clean plate与key组织。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)。

91. Adobe，[*Keying*](https://helpx.adobe.com/after-effects/desktop/animate-in-after-effects/keying/keying.html)，2024-10-24；key、cleaner与spill顺序。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)、[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

92. Adobe，[*Adjust Content-Aware Fill settings*](https://helpx.adobe.com/photoshop/desktop/repair-retouch/remove-objects-fill-space/adjust-content-aware-fill-settings.html)，2026-02-23；采样区等功能。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)、[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

93. Barnes等，[*PatchMatch: A Randomized Correspondence Algorithm for Structural Image Editing*](https://research.adobe.com/publication/patchmatch-a-randomized-correspondence-algorithm-for-structural-image-editing/)，Adobe Research，2009-08-02。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)。

94. Pérez等，[*Poisson Image Editing*](https://www.inf.ed.ac.uk/publications/report/1094.html)，2003；作者机构原始论文记录。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)。

95. CELSYS，[*Advanced Fill*](https://help.clip-studio.com/en-us/manual_en/420_fill/Advanced_Fill.htm)、[*Other Layer Filters*](https://help.clip-studio.com/en-us/manual_en/390_filters/Other_Layer_Filters.htm)，CSP5.0在线手册；填充与插画处理。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)、[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

96. CELSYS，[*Advanced Fill*](https://help.clip-studio.com/en-us/manual_en/420_fill/Advanced_Fill.htm)、[*Other Layer Filters*](https://help.clip-studio.com/en-us/manual_en/390_filters/Other_Layer_Filters.htm)，CSP5.0在线手册；填充与插画处理。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)。

97. CELSYS，[*User Manual*](https://help.clip-studio.com/en-us/)，在线版5.0；[Advanced Fill](https://help.clip-studio.com/en-us/manual_en/420_fill/Advanced_Fill.htm)、[Smart Tools](https://help.clip-studio.com/en-us/manual_en/390_filters/Smart_Tools.htm)。

   用于：[09-composite/keying-paint.md](../09-composite/keying-paint.md)、[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

98. Foundry，[*Using Scopes*](https://learn.foundry.com/nuke/content/timeline_environment/usingviewer/using_scopes.html)，滚动官方指南。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

99. Adobe，[*Color Basics*](https://helpx.adobe.com/mena_en/after-effects/desktop/adjust-colors/color-basics/color-basics.html)，英文AE指南。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

100. ITU-R，[*BT.709-6*](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf)，2015-06，Digital representation。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

101. SciPy，[*periodogram*](https://docs.scipy.org/doc/scipy-1.16.0/reference/generated/scipy.signal.periodogram.html)，1.16.0；[*welch*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.welch.html)，访问1.18.0。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

102. SciPy，[*periodogram*](https://docs.scipy.org/doc/scipy-1.16.0/reference/generated/scipy.signal.periodogram.html)，1.16.0；[*welch*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.welch.html)，访问1.18.0。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

103. Sharma、Wu、Dalal，[*CIEDE2000 supplementary test notes*](https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/ciede2000noteCRNA.pdf)，2005。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

104. [ImageBufAlgo](https://openimageio.readthedocs.io/en/stable/imagebufalgo.html)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[10-analysis/scopes.md](../10-analysis/scopes.md)。

105. [DNG p.99](https://helpx.adobe.com/content/dam/help/en/photoshop/pdf/DNG_Spec_1_7_1_0.pdf)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

106. [LibRaw数据结构](https://www.libraw.org/node/31)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

107. Adobe，[*HDR photo merge*](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/hdr-photo-merge.html)、[*Panorama*](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/panorama.html)，滚动Lightroom Classic指南。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

108. Adobe，[*HDR photo merge*](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/hdr-photo-merge.html)、[*Panorama*](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/panorama.html)，滚动Lightroom Classic指南。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

109. Adobe，[*Create a composite with extended depth of field*](https://helpx.adobe.com/photoshop/desktop/create-masks/blend-images/create-a-composite-with-extended-depth-of-field.html)，2026-02-23。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

110. Debevec、Malik，[*Recovering High Dynamic Range Radiance Maps from Photographs*](https://people.eecs.berkeley.edu/~malik/papers/debevec-malik97.pdf)，SIGGRAPH1997，作者论文。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

111. Mertens、Kautz、Van Reeth，[*Exposure Fusion: A Simple and Practical Alternative to High Dynamic Range Photography*](https://onlinelibrary.wiley.com/doi/10.1111/j.1467-8659.2008.01171.x)，2009。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

112. [SDK入口](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html)。发布方/日期以链接原文及采用文档为准；未注明日期时不推定。

   用于：[11-raw-photo/pipeline.md](../11-raw-photo/pipeline.md)。

113. Foundry，[*Time Nodes*](https://learn.foundry.com/nuke/content/reference_guide/time_nodes/time_nodes.html)，滚动指南。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

114. Adobe，[*Time Effects*](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/time-effects.html)，滚动英文指南。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)、[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

115. OpenCV，[*Optical Flow*](https://docs.opencv.org/4.x/d4/dee/tutorial_optical_flow.html)，4.x官方教程。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

116. Psyop，[*Cryptomatte specification and implementation*](https://github.com/Psyop/Cryptomatte)，仓库访问版本1.4.0。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

117. Foundry，[*CopyCat*](https://learn.foundry.com/nuke/current/content/reference_guide/air_nodes/copycat.html)、[*BigCat*](https://learn.foundry.com/nuke/content/reference_guide/air_nodes/bigcat.html)、[*Inference17.0*](https://learn.foundry.com/nuke/17.0/content/reference_guide/air_nodes/inference.html)。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

118. Foundry，[*CopyCat*](https://learn.foundry.com/nuke/current/content/reference_guide/air_nodes/copycat.html)、[*BigCat*](https://learn.foundry.com/nuke/content/reference_guide/air_nodes/bigcat.html)、[*Inference17.0*](https://learn.foundry.com/nuke/17.0/content/reference_guide/air_nodes/inference.html)。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

119. Foundry，[*CopyCat*](https://learn.foundry.com/nuke/current/content/reference_guide/air_nodes/copycat.html)、[*BigCat*](https://learn.foundry.com/nuke/content/reference_guide/air_nodes/bigcat.html)、[*Inference17.0*](https://learn.foundry.com/nuke/17.0/content/reference_guide/air_nodes/inference.html)。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

120. Zeng等，[*Image-Adaptive-3DLUT*](https://github.com/HuiZeng/Image-Adaptive-3DLUT)，TPAMI2022；Gharbi等，[*HDRNet*](https://groups.csail.mit.edu/graphics/hdrnet/)，SIGGRAPH2017；Zhang等，[*Colorful Image Colorization*](https://richzhang.github.io/colorization/)，ECCV2016。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

121. Zeng等，[*Image-Adaptive-3DLUT*](https://github.com/HuiZeng/Image-Adaptive-3DLUT)，TPAMI2022；Gharbi等，[*HDRNet*](https://groups.csail.mit.edu/graphics/hdrnet/)，SIGGRAPH2017；Zhang等，[*Colorful Image Colorization*](https://richzhang.github.io/colorization/)，ECCV2016。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

122. Zeng等，[*Image-Adaptive-3DLUT*](https://github.com/HuiZeng/Image-Adaptive-3DLUT)，TPAMI2022；Gharbi等，[*HDRNet*](https://groups.csail.mit.edu/graphics/hdrnet/)，SIGGRAPH2017；Zhang等，[*Colorful Image Colorization*](https://richzhang.github.io/colorization/)，ECCV2016。

   用于：[12-temporal-ml/requirements.md](../12-temporal-ml/requirements.md)。

123. Adobe，[*Photoshop What's New*](https://helpx.adobe.com/photoshop/desktop/whats-new/whats-new-in-adobe-photoshop-on-desktop.html)，2026-08-28；[Content-Aware Fill settings](https://helpx.adobe.com/photoshop/desktop/repair-retouch/remove-objects-fill-space/adjust-content-aware-fill-settings.html)，2026-02-23。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

124. Adobe，[*Lightroom Classic What's New*](https://helpx.adobe.com/lightroom-classic/desktop/introduction-to-lightroom-classic/whats-new.html)、[*Lightroom desktop What's New*](https://helpx.adobe.com/lightroom/desktop/introduction/whats-new.html)，2026-08；[Masking](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/masking.html)。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

125. Adobe，[*Lightroom Classic What's New*](https://helpx.adobe.com/lightroom-classic/desktop/introduction-to-lightroom-classic/whats-new.html)、[*Lightroom desktop What's New*](https://helpx.adobe.com/lightroom/desktop/introduction/whats-new.html)，2026-08；[Masking](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/masking.html)。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

126. Adobe，[*Lightroom Classic What's New*](https://helpx.adobe.com/lightroom-classic/desktop/introduction-to-lightroom-classic/whats-new.html)、[*Lightroom desktop What's New*](https://helpx.adobe.com/lightroom/desktop/introduction/whats-new.html)，2026-08；[Masking](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/masking.html)。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

127. Adobe，[*Camera Raw Release Notes*](https://helpx.adobe.com/camera-raw/desktop/whats-new/release-notes.html)，2026-08-27；[Enhance](https://helpx.adobe.com/camera-raw/desktop/edit-and-enhance-images/sharpening-and-noise/enhance.html)，2024-10-14。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

128. Adobe，[*Camera Raw Release Notes*](https://helpx.adobe.com/camera-raw/desktop/whats-new/release-notes.html)，2026-08-27；[Enhance](https://helpx.adobe.com/camera-raw/desktop/edit-and-enhance-images/sharpening-and-noise/enhance.html)，2024-10-14。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

129. Blackmagic Design，[*Photo*](https://www.blackmagicdesign.com/products/davinciresolve/photo)、[*Color*](https://www.blackmagicdesign.com/products/davinciresolve/color)，页面Resolve21；[21 New Features Guide](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_21_New_Features_Guide.pdf)，2026-04，具体功能与正式发行状态分开。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

130. Blackmagic Design，[*Photo*](https://www.blackmagicdesign.com/products/davinciresolve/photo)、[*Color*](https://www.blackmagicdesign.com/products/davinciresolve/color)，页面Resolve21；[21 New Features Guide](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_21_New_Features_Guide.pdf)，2026-04，具体功能与正式发行状态分开。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

131. CELSYS，[*User Manual*](https://help.clip-studio.com/en-us/)，在线版5.0；[Advanced Fill](https://help.clip-studio.com/en-us/manual_en/420_fill/Advanced_Fill.htm)、[Smart Tools](https://help.clip-studio.com/en-us/manual_en/390_filters/Smart_Tools.htm)。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

132. Foundry，[*Nuke17.1v1 Release Notes*](https://learn.foundry.com/nuke/content/release_notes/17.1/nuke_17.1v1_releasenotes.html)，2026-08-20；[17.0v3](https://learn.foundry.com/nuke/content/release_notes/17.0/nuke_17.0v3_releasenotes.html)，2026-06-09，legacy OFX移除。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

133. Foundry，[*Nuke17.1v1 Release Notes*](https://learn.foundry.com/nuke/content/release_notes/17.1/nuke_17.1v1_releasenotes.html)，2026-08-20；[17.0v3](https://learn.foundry.com/nuke/content/release_notes/17.0/nuke_17.0v3_releasenotes.html)，2026-06-09，legacy OFX移除。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。

134. Adobe，[*After Effects Release Notes*](https://helpx.adobe.com/after-effects/desktop/what-s-new/release-notes-after-effects.html)，2026-06-17；[Keying](https://helpx.adobe.com/after-effects/desktop/animate-in-after-effects/keying/keying.html)、[Time Effects](https://helpx.adobe.com/after-effects/desktop/apply-effects-and-animation-presets/list-of-effects/time-effects.html)。

   用于：[13-coverage/software-matrix.md](../13-coverage/software-matrix.md)。
