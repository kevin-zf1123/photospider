#include "plugin_api.hpp"
#include "ps_types.hpp"
#include "metal/perlin_noise_metal.hpp"

// 每个插件都需要导出一个 C 风格的函数 `register_photospider_ops`
// extern "C" 确保 C++ 编译器不会对函数名进行修饰 (name mangling)
extern "C" PLUGIN_API void register_photospider_ops() {
    auto& R = ps::OpRegistry::instance();

    // [新增] 为 Metal 操作创建元数据，并明确指定其设备偏好
    ps::OpMetadata metal_meta;
    metal_meta.device_preference = ps::Device::GPU_METAL;
    // TODO: 如果此操作未来支持分块，还需设置 tile_preference

    // 注册 Metal Perlin Noise 操作
    R.register_op("image_generator", "perlin_noise_metal", 
                  ps::ops::op_perlin_noise_metal, 
                  metal_meta);

    // 预热 Metal 状态，避免首次并行初始化竞争
    perlin_noise_metal_eager_init();
}
