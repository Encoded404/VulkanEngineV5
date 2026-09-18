[[vk::binding(0, 5)]]
Sampler2D sceneColor;

[[vk::binding(1, 5)]]
RWStructuredBuffer<float> exposure;

// Single-thread estimate of scene exposure, stored in a transient buffer that
// the tonemap pass reads. Exercises the transient-buffer path and the
// compute -> graphics hazard edge.
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float3 color = sceneColor.SampleLevel(float2(0.5, 0.5), 0).rgb;
    const float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    exposure[0] = 1.0 / max(luma, 0.05);
}
