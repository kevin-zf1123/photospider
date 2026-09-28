struct Parameters {
    uint width;
    uint height;
    uint depth;
    uint multiplier;
    uint increment;
};
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> values;
[[vk::binding(1, 0)]] ConstantBuffer<Parameters> parameters;
[numthreads(8, 4, 2)]
[shader("compute")]
void transform(uint3 at : SV_DispatchThreadID) {
    if (at.x >= parameters.width || at.y >= parameters.height || at.z >= parameters.depth)
        return;
    uint index = at.x + parameters.width * (at.y + parameters.height * at.z);
    uint64_t wide = uint64_t(values[index]) * parameters.multiplier + parameters.increment;
    values[index] = uint(wide ^ (wide >> 32));
}

[numthreads(1, 1, 1)]
[shader("compute")]
void numeric_probe() {
    uint tiny = values[0];
    uint normal = values[1];
    values[2] = tiny;
    values[3] = asuint(asfloat(normal) * 0.5f);
    values[4] = asuint(asfloat(tiny) + asfloat(tiny));
    values[5] = asuint(sqrt(asfloat(normal)));
    values[6] = asuint(sqrt(asfloat(tiny)));
}
