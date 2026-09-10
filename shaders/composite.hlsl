Texture2D<float4> colorTexture : register(t0);
Texture2D<float4> materialTexture : register(t1);
SamplerState linearClamp : register(s0);

cbuffer CompositeConstants : register(b1)
{
    float4 fallback;
    uint fallbackEnabled;
    float3 padding;
};

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    static const float2 positions[6] =
    {
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2(-1.0, -1.0)
    };
    static const float2 uvs[6] =
    {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 1.0)
    };

    FullscreenOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float4 CompositePS(FullscreenOutput input) : SV_TARGET
{
    float4 result = colorTexture.Sample(linearClamp, input.uv);

    // Keep the material resource bound as part of the compositor contract;
    // it is available for a future edge-aware upscale pass.
    float4 material = materialTexture.Sample(linearClamp, input.uv);
    result.rgb += material.rgb * 0.0;

    if(fallbackEnabled != 0u && result.a < 0.001)
    {
        float2 delta = (input.uv - fallback.xy) * float2(fallback.w, 1.0);
        float radial = length(delta) / max(fallback.z, 1.0e-5);
        if(radial > 1.30) discard;
        if(radial < 0.78)
            return float4(0.0, 0.0, 0.0, 1.0);

        float ringT = saturate(
            (radial - 0.78) / (1.30 - 0.78));
        float3 innerColor = float3(1.0, 0.15, 0.01);
        float3 outerColor = float3(1.0, 0.72, 0.10);
        float3 ringColor = lerp(innerColor, outerColor, ringT);
        float alpha = 0.92 * (1.0 - smoothstep(0.78, 1.30, radial));
        return float4(ringColor * alpha, alpha);
    }

    return result;
}
