[[vk::binding(0, 0)]] RWStructuredBuffer<uint8_t> bytes;
struct Samples {
    uint64_t values[70];
};
[[vk::binding(1, 0)]] ConstantBuffer<Samples> samples;
[shader("compute")]
[numthreads(64, 1, 1)]
void produce(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= 70)
        return;
    const uint at = 49 + (i / 35) * 320 + ((i % 35) / 7) * 56 - (i % 7) * 8;
    for (uint k = 0; k < 8; ++k)
        bytes[at + k] = uint8_t(samples.values[i] >> (8 * k));
}
