[[vk::binding(0, 0)]] RWStructuredBuffer<uint8_t> bytes;
struct Coordinates {
    uint64_t values[15];
};
[[vk::binding(1, 0)]] ConstantBuffer<Coordinates> coordinates;
[shader("compute")]
[numthreads(64, 1, 1)]
void produce(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= 15)
        return;
    const uint at = 65 + (i / 3) * 32 - (i % 3) * 8;
    for (uint k = 0; k < 8; ++k)
        bytes[at + k] = uint8_t(coordinates.values[i] >> (8 * k));
}
