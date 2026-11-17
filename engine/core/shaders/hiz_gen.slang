#define BATCH_LEVELS 2
#define STRIDE (1 << BATCH_LEVELS)
#define HALF (STRIDE >> 1)

// Generates two Hi-Z mip levels per dispatch by 4x4 → 2x2 → 1x1 reduction (max-depth).
// One thread consumes a STRIDE x STRIDE tile of the source level, so the caller dispatches the tile grid (source extent / STRIDE, rounded up).
// A source-extent dispatch leaves 15/16 of the threads on the guard below.
// The caller mirrors this tile size as SceneRenderer::HIZ_TILE_STRIDE.
[[vk::binding(0, 0)]]
Texture2D<float> depthTexture;
[[vk::binding(1, 0)]]
SamplerState depthSampler;
[[vk::binding(2, 0)]]
globallycoherent RWTexture2D<float> hizLevels[];

struct HiZPC {
    uint baseLevel;
    uint srcWidth;
    uint srcHeight;
    uint totalLevels;
};

[[vk::push_constant]]
ConstantBuffer<HiZPC> pc;

[numthreads(16, 16, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    int2 srcSize = int2(pc.srcWidth, pc.srcHeight);
    int2 threadPos = int2(dispatchThreadId.xy);
    int2 coord = threadPos * STRIDE;

    if (coord.x < srcSize.x && coord.y < srcSize.y) {
        float depthValues[STRIDE][STRIDE];

        // Keep the source branch outside the load loop.
        // Inside the loop, each unrolled load carries its own uniform branch, which prevents the loads from being scheduled together.
        // Unroll the tile and the reduction loops as well, so every depthValues access stays constant-indexed and the compiler keeps the tile in registers.
        if (pc.baseLevel == 0) {
            [ForceUnroll]
            for (int y = 0; y < STRIDE; y++) {
                [ForceUnroll]
                for (int x = 0; x < STRIDE; x++) {
                    int2 clampedCoord = min(coord + int2(x, y), srcSize - 1);
                    depthValues[y][x] = depthTexture.Load(int3(clampedCoord, 0)).r;
                }
            }
        } else {
            const uint srcLevel = pc.baseLevel - 1;
            [ForceUnroll]
            for (int y = 0; y < STRIDE; y++) {
                [ForceUnroll]
                for (int x = 0; x < STRIDE; x++) {
                    int2 clampedCoord = min(coord + int2(x, y), srcSize - 1);
                    depthValues[y][x] = hizLevels[srcLevel][clampedCoord];
                }
            }
        }

        int2 lvl0Size = (pc.baseLevel == 0)
            ? max(int2(1, 1), (srcSize + 1) / 2)
            : max(int2(1, 1), srcSize >> 1);

        // first reduction: 4x4 -> 2x2 (writes dstLevel)
        {
            uint dstLevel = pc.baseLevel;
            [ForceUnroll]
            for (int y = 0; y < HALF; y++) {
                [ForceUnroll]
                for (int x = 0; x < HALF; x++) {
                    float reducedDepth = max(
                        max(depthValues[y * 2][x * 2], depthValues[y * 2 + 1][x * 2]),
                        max(depthValues[y * 2][x * 2 + 1], depthValues[y * 2 + 1][x * 2 + 1])
                    );
                    int2 writePos = threadPos * HALF + int2(x, y);
                    if (dstLevel < pc.baseLevel + pc.totalLevels && writePos.x < lvl0Size.x && writePos.y < lvl0Size.y)
                        hizLevels[dstLevel][writePos] = reducedDepth;
                    depthValues[y][x] = reducedDepth;
                }
            }
        }

        // second reduction: 2x2 -> 1x1 (writes dstLevel + 1)
        {
            uint dstLevel = pc.baseLevel + 1;
            int2 lvl1Size = max(int2(1, 1), lvl0Size >> 1);
            float reducedDepth = max(max(depthValues[0][0], depthValues[1][0]), max(depthValues[0][1], depthValues[1][1]));
            if (dstLevel < pc.baseLevel + pc.totalLevels && threadPos.x < lvl1Size.x && threadPos.y < lvl1Size.y)
                hizLevels[dstLevel][threadPos] = reducedDepth;
        }
    }
}
