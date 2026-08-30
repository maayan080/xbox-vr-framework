// Reference content for the framework: a small 3D room, rendered from the headset's
// reported viewpoint.
//
// It is deliberately a scene with depth rather than a flat shape. Head tracking is only
// visible as parallax - near objects moving further across the view than far ones - so a
// flat image would render the whole pose link untestable.
//
// Geometry comes from SV_VertexID: no vertex buffer, no input layout, nothing that can
// fall out of step with the shader.

cbuffer SceneParams : register(b0)
{
    float4x4 g_viewProjection;
    float4 g_params; // x = time, y = eye index, z/w unused
    // Per hand: xyz = grip position, w = active flag.
    float4 g_handPosition[2];
    // Per hand: grip orientation as a quaternion.
    float4 g_handOrientation[2];
    // Per hand: x = trigger, y = squeeze, zw = thumbstick.
    float4 g_handAnalog[2];
    // Per hand: x holds the button bitmask as a float value (0/1 per indicator is decoded
    // on the host before upload, to keep bit twiddling out of the shader).
    float4 g_handButtons[2];
};

float3 RotateByQuaternion(float3 v, float4 q)
{
    float3 u = q.xyz;
    return v + 2.0 * cross(u, cross(u, v) + q.w * v);
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 colour : COLOR;
};

static const float3 kCubeCorners[8] = {
    float3(-1, -1, -1), float3( 1, -1, -1), float3( 1,  1, -1), float3(-1,  1, -1),
    float3(-1, -1,  1), float3( 1, -1,  1), float3( 1,  1,  1), float3(-1,  1,  1),
};

// Two triangles per face, six faces.
static const uint kCubeIndices[36] = {
    0, 2, 1, 0, 3, 2,
    1, 6, 5, 1, 2, 6,
    5, 7, 4, 5, 6, 7,
    4, 3, 0, 4, 7, 3,
    3, 6, 2, 3, 7, 6,
    4, 1, 5, 4, 0, 1,
};

// Position, scale and colour of each object. Spread in depth so parallax is obvious.
static const float4 kObjects[6] = {
    float4( 0.0, 0.0, -2.0, 0.25),
    float4(-1.2, 0.3, -3.5, 0.35),
    float4( 1.4, -0.2, -4.5, 0.40),
    float4(-0.6, -0.5, -6.0, 0.30),
    float4( 1.0, 0.6, -8.0, 0.55),
    float4(-2.0, 0.1, -10.0, 0.70),
};

static const float3 kObjectColours[6] = {
    float3(1.00, 0.35, 0.30),
    float3(0.35, 0.95, 0.45),
    float3(0.40, 0.55, 1.00),
    float3(1.00, 0.85, 0.30),
    float3(0.85, 0.40, 1.00),
    float3(0.30, 0.90, 0.90),
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;

    const uint kCubeVertexCount = 36;
    const uint kObjectCount = 6;
    const uint kFloorStart = kCubeVertexCount * kObjectCount;
    const uint kGridQuads = 20 * 20;
    const uint kHandStart = kFloorStart + kGridQuads * 6;

    // Hands: a grey cube at each grip pose, with a row of indicators underneath showing
    // which inputs are pressed. Drawn last so the vertex ranges above are untouched.
    if (vertexId >= kHandStart)
    {
        const uint kIndicatorCount = 6;
        const uint kHandVertices = kCubeVertexCount + kIndicatorCount * 6;

        uint local = vertexId - kHandStart;
        uint hand = local / kHandVertices;
        local = local % kHandVertices;

        float4 position = g_handPosition[hand];
        float4 orientation = g_handOrientation[hand];

        // Inactive controllers collapse to nothing rather than sitting at the origin,
        // where a stale cube would look like a tracked hand that had stopped moving.
        if (position.w < 0.5)
        {
            output.position = float4(0.0, 0.0, -1.0, 0.0);
            output.colour = float3(0.0, 0.0, 0.0);
            return output;
        }

        if (local < kCubeVertexCount)
        {
            float3 corner = kCubeCorners[kCubeIndices[local]] * 0.045;
            float3 world = position.xyz + RotateByQuaternion(corner, orientation);

            output.position = mul(g_viewProjection, float4(world, 1.0));

            // Shaded by face so the cube reads as solid rather than a flat silhouette.
            float shade = 0.55 + 0.45 * float(local / 6) / 5.0;
            output.colour = float3(0.62, 0.64, 0.68) * shade;
            return output;
        }

        // Indicator strip: one square per input, lit when pressed. Analogue inputs light
        // proportionally, so a half-pulled trigger is visibly half lit.
        uint indicator = (local - kCubeVertexCount) / 6;
        uint corner = (local - kCubeVertexCount) % 6;

        const float2 quad[6] = {
            float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
            float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
        };

        const float size = 0.018;
        const float gap = 0.006;
        const float totalWidth = kIndicatorCount * size + (kIndicatorCount - 1) * gap;

        float2 local2 = quad[corner];
        float x = -totalWidth * 0.5 + indicator * (size + gap) + local2.x * size;
        float y = -0.075 - size + local2.y * size;

        // Billboarded on the horizontal axes only: the strip stays readable as the hand
        // rotates, without tumbling with it.
        float3 offset = float3(x, y, 0.0);
        float3 world = position.xyz + offset;

        output.position = mul(g_viewProjection, float4(world, 1.0));

        float level = g_handButtons[hand][0];
        float value = 0.0;
        if (indicator == 0) { value = g_handAnalog[hand].x; }                       // trigger
        else if (indicator == 1) { value = g_handAnalog[hand].y; }                  // squeeze
        else if (indicator == 2) { value = fmod(floor(level / 1.0), 2.0); }         // primary
        else if (indicator == 3) { value = fmod(floor(level / 2.0), 2.0); }         // secondary
        else if (indicator == 4) { value = fmod(floor(level / 4.0), 2.0); }         // menu
        else { value = fmod(floor(level / 8.0), 2.0); }                             // stick click

        float3 dim = float3(0.16, 0.17, 0.20);
        float3 lit = (indicator < 2) ? float3(0.30, 0.85, 1.00) : float3(1.00, 0.80, 0.25);
        output.colour = lerp(dim, lit, saturate(value));
        return output;
    }

    if (vertexId < kFloorStart)
    {
        const uint objectIndex = vertexId / kCubeVertexCount;
        const uint localVertex = vertexId % kCubeVertexCount;

        float4 object = kObjects[objectIndex];

        // A slow rotation, so a frozen stream is distinguishable from a live one.
        float angle = g_params.x * 0.6 + float(objectIndex);
        float c = cos(angle);
        float s = sin(angle);

        float3 corner = kCubeCorners[kCubeIndices[localVertex]] * object.w;
        float3 rotated = float3(corner.x * c - corner.z * s, corner.y,
                                corner.x * s + corner.z * c);

        float3 world = object.xyz + rotated;
        output.position = mul(g_viewProjection, float4(world, 1.0));

        // Shade by face so the cubes read as solid rather than as flat silhouettes.
        float shade = 0.55 + 0.45 * float(localVertex / 6) / 5.0;
        output.colour = kObjectColours[objectIndex] * shade;
        return output;
    }

    // Floor grid: a fan of quads gives the eye a continuous surface to judge depth
    // against, which is what makes head motion read as motion rather than as wobble.
    const uint gridVertex = vertexId - kFloorStart;
    const uint quadIndex = gridVertex / 6;
    const uint corner = gridVertex % 6;

    const uint kGridSize = 20;
    const uint gx = quadIndex % kGridSize;
    const uint gz = quadIndex / kGridSize;

    const float cell = 0.75;
    const float originX = -float(kGridSize) * cell * 0.5;
    const float originZ = -float(kGridSize) * cell;

    const float2 offsets[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1),
    };

    float2 local = offsets[corner];
    float3 world = float3(originX + (float(gx) + local.x) * cell, -1.2,
                          originZ + (float(gz) + local.y) * cell);

    output.position = mul(g_viewProjection, float4(world, 1.0));

    // Checkerboard, which makes translation obvious in a way a flat colour cannot.
    float checker = fmod(float(gx + gz), 2.0);
    float fade = saturate(1.0 - float(gz) / float(kGridSize));
    output.colour = lerp(float3(0.10, 0.12, 0.16), float3(0.22, 0.26, 0.34), checker) *
                    (0.35 + 0.65 * fade);
    return output;
}
