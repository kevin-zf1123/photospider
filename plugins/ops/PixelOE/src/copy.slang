[shader("compute")]
[numthreads(256, 1, 1)]
void copy_words(uint3 tid: SV_DispatchThreadID,
                uniform StructuredBuffer<uint> source,
                uniform RWStructuredBuffer<uint> destination,
                uniform uint64_t count) {
    if (uint64_t(tid.x) < count)
        destination[tid.x] = source[tid.x];
}
