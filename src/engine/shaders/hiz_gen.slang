#define BATCH_LEVELS 2
#define STRIDE (1 << BATCH_LEVELS)
#define HALF (STRIDE >> 1)

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
    int2 pos = int2(dispatchThreadId.xy);
    int2 coord = pos * STRIDE;

    if (coord.x < srcSize.x && coord.y < srcSize.y) {
        float d[STRIDE][STRIDE];

        for (int y = 0; y < STRIDE; y++) {
            for (int x = 0; x < STRIDE; x++) {
                int2 c = min(coord + int2(x, y), srcSize - 1);
                if (pc.baseLevel == 0)
                    d[y][x] = depthTexture.Load(int3(c, 0)).r;
                else
                    d[y][x] = hizLevels[pc.baseLevel - 1][c];
            }
        }

        int2 lvl0Size = (pc.baseLevel == 0)
            ? max(int2(1, 1), (srcSize + 1) / 2)
            : max(int2(1, 1), srcSize >> 1);

        {
            uint dstLvl = pc.baseLevel;
            for (int y = 0; y < HALF; y++) {
                for (int x = 0; x < HALF; x++) {
                    float v = max(
                        max(d[y * 2][x * 2], d[y * 2 + 1][x * 2]),
                        max(d[y * 2][x * 2 + 1], d[y * 2 + 1][x * 2 + 1])
                    );
                    int2 wPos = pos * HALF + int2(x, y);
                    if (dstLvl < pc.baseLevel + pc.totalLevels && wPos.x < lvl0Size.x && wPos.y < lvl0Size.y)
                        hizLevels[dstLvl][wPos] = v;
                    d[y][x] = v;
                }
            }
        }

        {
            uint dstLvl = pc.baseLevel + 1;
            int2 lvl1Size = max(int2(1, 1), lvl0Size >> 1);
            float v = max(max(d[0][0], d[1][0]), max(d[0][1], d[1][1]));
            if (dstLvl < pc.baseLevel + pc.totalLevels && pos.x < lvl1Size.x && pos.y < lvl1Size.y)
                hizLevels[dstLvl][pos] = v;
        }
    }
}
