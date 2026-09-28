[[vk::binding(0, 0)]] StructuredBuffer<float> input_values;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> output_values;
[numthreads(1, 1, 1)]
[shader("compute")]
void scale(uint3 at : SV_DispatchThreadID) {
    output_values[at.x] = input_values[at.x] * 0.5f;
}
