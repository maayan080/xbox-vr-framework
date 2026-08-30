#include "XvrVr.h"

#include <GLES2/gl2ext.h>

#include <cmath>

namespace xvr {
namespace {

// A virtual screen placed in front of the viewer, in metres.
//
// Both eyes draw it at the same world position, which is what lets them fuse. It sits at
// a fixed point in the LOCAL reference space rather than following the head, so looking
// around also demonstrates that tracking works.
// Roughly 50 degrees horizontal, which is a comfortable size for a virtual monitor. Wider
// than about 60 asks the eyes to keep scanning to the periphery, which is tiring on its own
// regardless of how correct the stereo is.
constexpr float kScreenDistance = 3.0f;
constexpr float kScreenWidth = 2.8f;

// Assumed scene distance for positional timewarp, metres.
//
// Translation cannot be corrected exactly without per-pixel depth, so one representative
// distance is used instead: exact at that depth, progressively wrong away from it. Chosen
// toward the near end of the scene because close objects exhibit the most parallax and so
// are where the error is most visible - distant content barely moves either way.
constexpr float kFocusDepth = 2.5f;
constexpr float kScreenAspect = 1920.0f / 1088.0f;

// Quad corners generated from gl_VertexID as two triangles: no vertex buffer to keep in
// step with the shader.
const char* kQuadVertex = R"GLSL(#version 300 es
uniform mat4 uMvp;
uniform vec4 uScreen; // halfWidth, halfHeight, distance, unused
out vec2 vUv;
void main() {
    vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
        vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));
    vec2 c = corners[gl_VertexID];

    vUv = vec2((c.x + 1.0) * 0.5, 1.0 - (c.y + 1.0) * 0.5);

    vec4 world = vec4(c.x * uScreen.x, c.y * uScreen.y, -uScreen.z, 1.0);
    gl_Position = uMvp * world;
}
)GLSL";

// Video fills the eye completely rather than being drawn on a screen in the world: the
// host renders each frame using the exact per-eye pose and frustum this client reported,
// so the decoded image already *is* this eye's view.
const char* kFullscreenVertex = R"GLSL(#version 300 es
out vec2 vNdc;
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[gl_VertexID];
    vNdc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

// Asynchronous timewarp.
//
// The frame on screen was rendered for where the head was when the host started it - one
// full round trip ago: pose out, render, encode, network, decode. Displaying it as though
// it were current is what makes head motion feel laggy and stuttery, because every frame
// is late by a different amount.
//
// Instead, for each pixel this works out which direction the eye is looking *now*, rotates
// that direction back into the frame's original orientation, and samples where that
// direction landed in the rendered image. The result is the old frame re-aimed at the
// current head pose. It corrects rotation almost exactly, which is the dominant and most
// uncomfortable component; translation would need per-pixel depth and is not attempted.
//
// Pixels that fall outside the rendered frustum have no data - the host never drew them -
// so they are faded rather than clamped, which reads as a soft edge instead of smeared
// streaks.
const char* kVideoFragment = R"GLSL(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

in vec2 vNdc;
uniform samplerExternalOES uTexture;
// Rotation from current eye space into the space the frame was rendered in.
uniform mat3 uReprojection;
// Current eye orientation (eye space -> world) and world -> render eye space.
uniform mat3 uCurrentRotation;
uniform mat3 uRenderRotationInv;
// Eye positions in the client's tracking space, metres.
uniform vec3 uCurrentPosition;
uniform vec3 uRenderPosition;
// Assumed scene distance used to correct translation, metres. Zero disables it.
uniform float uFocusDepth;
// tan of the frustum half-angles: left, right, up, down.
uniform vec4 uCurrentFov;
uniform vec4 uRenderFov;
out vec4 fragColour;

void main() {
    // Direction this pixel looks in, in the current eye's space.
    float x = mix(uCurrentFov.x, uCurrentFov.y, (vNdc.x + 1.0) * 0.5);
    float y = mix(uCurrentFov.w, uCurrentFov.z, (vNdc.y + 1.0) * 0.5);
    vec3 rayNow = vec3(x, y, -1.0);

    vec3 rayRender;

    if (uFocusDepth > 0.0) {
        // Positional correction. Rotation alone can be applied to a direction, but
        // translation only means something for a point - how far the image should shift
        // depends entirely on how distant the thing being looked at is. Without per-pixel
        // depth the best available answer is to assume a single representative distance:
        // exact for content at that depth, and progressively approximate away from it.
        // Leaving translation uncorrected instead makes the whole world slide with the
        // head, which is far more noticeable than the approximation's error.
        vec3 worldPoint = uCurrentPosition + (uCurrentRotation * normalize(rayNow)) * uFocusDepth;
        rayRender = uRenderRotationInv * (worldPoint - uRenderPosition);
    } else {
        // The same direction, expressed in the frame's original orientation.
        rayRender = uReprojection * rayNow;
    }

    // Anything at or behind the eye plane was never in frame.
    if (rayRender.z >= -0.0001) {
        fragColour = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 tanRender = rayRender.xy / -rayRender.z;

    vec2 uv;
    uv.x = (tanRender.x - uRenderFov.x) / (uRenderFov.y - uRenderFov.x);
    uv.y = (tanRender.y - uRenderFov.w) / (uRenderFov.z - uRenderFov.w);

    // Video arrives top-down while GL samples bottom-up.
    uv.y = 1.0 - uv.y;

    // Fade rather than clamp outside the rendered frustum: clamping smears edge pixels
    // across the periphery, which is far more distracting than a soft border.
    vec2 fade = smoothstep(vec2(0.0), vec2(0.02), uv) *
                (1.0 - smoothstep(vec2(0.98), vec2(1.0), uv));
    float edge = fade.x * fade.y;

    fragColour = vec4(texture(uTexture, clamp(uv, 0.0, 1.0)).rgb * edge, 1.0);
}
)GLSL";

// Shown until video is flowing. The colour encodes how far the pipeline got, so the
// headset itself reports the failure stage without needing logcat attached.
const char* kPatternFragment = R"GLSL(#version 300 es
precision mediump float;
in vec2 vUv;
uniform vec3 uColour;
uniform float uTime;
out vec4 fragColour;
void main() {
    float band = smoothstep(0.35, 0.5, abs(fract(vUv.y * 4.0 - uTime * 0.35) - 0.5));
    vec3 base = uColour * (0.35 + 0.65 * band);
    float grid = step(0.98, fract(vUv.x * 20.0)) + step(0.98, fract(vUv.y * 20.0));
    fragColour = vec4(base + grid * 0.15, 1.0);
}
)GLSL";

GLuint CompileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled)
    {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        XVR_ERR("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

GLuint GlRenderer::CompileProgram(const char* vertexSource, const char* fragmentSource)
{
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0)
    {
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[1024]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        XVR_ERR("program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}

bool GlRenderer::Initialize()
{
    // Video fills the eye; the status pattern stays on a world-locked quad, where a
    // "waiting" screen floating in a dark room is the honest representation of having
    // nothing to show.
    m_videoProgram = CompileProgram(kFullscreenVertex, kVideoFragment);
    m_patternProgram = CompileProgram(kQuadVertex, kPatternFragment);

    if (m_patternProgram == 0)
    {
        XVR_ERR("pattern program failed; nothing can be drawn");
        return false;
    }
    if (m_videoProgram == 0)
    {
        // Not fatal: the status pattern still renders, which tells us the external-texture
        // extension is the problem rather than the session.
        XVR_WARN("video program failed to build; external textures unavailable");
    }

    m_patternColourLocation = glGetUniformLocation(m_patternProgram, "uColour");
    m_patternTimeLocation = glGetUniformLocation(m_patternProgram, "uTime");
    m_patternMvpLocation = glGetUniformLocation(m_patternProgram, "uMvp");

    if (m_videoProgram != 0)
    {
        m_videoTextureLocation = glGetUniformLocation(m_videoProgram, "uTexture");
        m_videoReprojectionLocation = glGetUniformLocation(m_videoProgram, "uReprojection");
        m_videoCurrentRotationLocation =
            glGetUniformLocation(m_videoProgram, "uCurrentRotation");
        m_videoRenderRotationInvLocation =
            glGetUniformLocation(m_videoProgram, "uRenderRotationInv");
        m_videoCurrentPositionLocation =
            glGetUniformLocation(m_videoProgram, "uCurrentPosition");
        m_videoRenderPositionLocation = glGetUniformLocation(m_videoProgram, "uRenderPosition");
        m_videoFocusDepthLocation = glGetUniformLocation(m_videoProgram, "uFocusDepth");
        m_videoCurrentFovLocation = glGetUniformLocation(m_videoProgram, "uCurrentFov");
        m_videoRenderFovLocation = glGetUniformLocation(m_videoProgram, "uRenderFov");
    }

    // Created on the render thread's context so the SurfaceTextures Java wraps around them
    // are usable here. A texture from another context would silently draw nothing.
    glGenTextures(2, m_eyeTextures);
    for (GLuint texture : m_eyeTextures)
    {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    glGenVertexArrays(1, &m_vertexArray);

    XVR_LOG("renderer ready (eye textures %u, %u)", m_eyeTextures[0], m_eyeTextures[1]);
    return true;
}

void GlRenderer::Shutdown()
{
    if (m_vertexArray != 0)
    {
        glDeleteVertexArrays(1, &m_vertexArray);
        m_vertexArray = 0;
    }
    glDeleteTextures(2, m_eyeTextures);
    m_eyeTextures[0] = m_eyeTextures[1] = 0;

    if (m_videoProgram != 0)
    {
        glDeleteProgram(m_videoProgram);
        m_videoProgram = 0;
    }
    if (m_patternProgram != 0)
    {
        glDeleteProgram(m_patternProgram);
        m_patternProgram = 0;
    }
}

void GlRenderer::SetReprojection(int eye, const Warp& warp)
{
    if (eye >= 0 && eye <= 1)
    {
        m_reprojection[eye] = warp;
    }
}

void GlRenderer::DrawEye(int eye, int width, int height, Status status, float timeSeconds,
                         const float* viewProjection)
{
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Everything outside the virtual screen is black, so the screen reads as an object in
    // a dark room rather than as an image pasted over the eye.
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindVertexArray(m_vertexArray);

    const float halfWidth = kScreenWidth * 0.5f;
    const float halfHeight = halfWidth / kScreenAspect;

    if (status == Status::Streaming && m_videoProgram != 0)
    {
        const Warp& warp = m_reprojection[eye];

        glUseProgram(m_videoProgram);
        glUniformMatrix3fv(m_videoReprojectionLocation, 1, GL_FALSE, warp.reprojection);
        glUniformMatrix3fv(m_videoCurrentRotationLocation, 1, GL_FALSE, warp.currentRotation);
        glUniformMatrix3fv(m_videoRenderRotationInvLocation, 1, GL_FALSE,
                           warp.renderRotationInv);
        glUniform3fv(m_videoCurrentPositionLocation, 1, warp.currentPosition);
        glUniform3fv(m_videoRenderPositionLocation, 1, warp.renderPosition);
        // Disabled when the frame's originating pose is unknown: correcting translation
        // against a guessed viewpoint would be worse than not correcting it.
        glUniform1f(m_videoFocusDepthLocation, warp.valid ? kFocusDepth : 0.0f);
        glUniform4fv(m_videoCurrentFovLocation, 1, warp.currentFovTan);
        glUniform4fv(m_videoRenderFovLocation, 1, warp.renderFovTan);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_eyeTextures[eye]);
        glUniform1i(m_videoTextureLocation, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        return;
    }

    // Colour-coded progress, so a failure stage is identifiable from inside the headset.
    float colour[3];
    switch (status)
    {
    case Status::WaitingForHost:
        colour[0] = 0.75f; colour[1] = 0.18f; colour[2] = 0.18f; // red: nothing arriving
        break;
    case Status::ReceivingNoKeyframe:
        colour[0] = 0.85f; colour[1] = 0.55f; colour[2] = 0.15f; // amber: data, no keyframe
        break;
    case Status::Decoding:
        colour[0] = 0.20f; colour[1] = 0.45f; colour[2] = 0.85f; // blue: decoding, no image
        break;
    default:
        colour[0] = 0.20f; colour[1] = 0.75f; colour[2] = 0.35f; // green
        break;
    }

    glUseProgram(m_patternProgram);
    glUniformMatrix4fv(m_patternMvpLocation, 1, GL_FALSE, viewProjection);
    glUniform4f(glGetUniformLocation(m_patternProgram, "uScreen"), halfWidth, halfHeight,
                kScreenDistance, 0.0f);
    glUniform3f(m_patternColourLocation, colour[0], colour[1], colour[2]);
    glUniform1f(m_patternTimeLocation, timeSeconds);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

} // namespace xvr
