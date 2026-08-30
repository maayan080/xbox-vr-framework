struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 tint : COLOR;
};

float4 main(PSInput input) : SV_TARGET
{
    // One fixed light, purely so the cube faces are distinguishable. Nothing here is
    // trying to look good - it exists to prove the pose and projection are right.
    const float3 lightDirection = normalize(float3(0.4f, 0.8f, 0.45f));
    float lambert = saturate(dot(normalize(input.normal), lightDirection));
    float shade = 0.25f + 0.75f * lambert;
    return float4(input.tint.rgb * shade, 1.0f);
}
