cbuffer GridConstants : register(b0)
{
    column_major float4x4 mvp;
    float4 color;
};

struct GridInput
{
    float3 position : POSITION;
};

struct GridOutput
{
    float4 position : SV_POSITION;
};

GridOutput GridVS(GridInput input)
{
    GridOutput output;
    output.position = mul(mvp, float4(input.position, 1.0));
    return output;
}

float4 GridPS(GridOutput input) : SV_TARGET
{
    return color;
}
