cbuffer PerObject : register(b0)
{
    float4x4 ModelViewProjection;
    float4 Tint;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 tint : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), ModelViewProjection);
    output.normal = input.normal;
    output.tint = Tint;
    return output;
}
