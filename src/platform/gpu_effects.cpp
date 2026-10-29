#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "platform/gpu_effects.h"
#include <cmath>

namespace {
constexpr const char* vertex = R"GLSL(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
constexpr const char* fragment = R"GLSL(#version 330 core
uniform sampler2D sourceImage;
uniform mat3 toWorld, toPixel;
uniform bool lightOn, pixelOn, quantizeOn, ditherOn, outlineOn;
uniform vec2 lightPosition;
uniform vec3 lightColor, outlineColor;
uniform float radius, intensity, ambient, grid;
uniform int colorCount;
uniform vec3 palette[256];
out vec4 result;
const int bayer[16] = int[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);

vec4 sampleCell(ivec2 pixel, out vec2 world, out vec2 cell) {
    world = (toWorld * vec3(vec2(pixel) + 0.5, 1.0)).xy;
    cell = floor(world / grid);
    if (pixelOn) {
        world = (cell + 0.5) * grid;
        pixel = ivec2(clamp(floor((toPixel * vec3(world, 1.0)).xy), vec2(0), vec2(textureSize(sourceImage, 0) - 1)));
    }
    vec4 color = texelFetch(sourceImage, pixel, 0);
    if (pixelOn) color.a = color.a >= 128.0 / 255.0 ? 1.0 : 0.0;
    return color;
}
vec3 nearestColor(vec3 rgb) {
    float best = 1e9;
    vec3 chosen = palette[0];
    for (int i = 0; i < colorCount; ++i) {
        vec3 delta = rgb - palette[i]; float d = dot(delta, delta);
        if (d < best) { best = d; chosen = palette[i]; }
    }
    return chosen;
}
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (!lightOn && !pixelOn) { result = texelFetch(sourceImage, pixel, 0); return; }
    vec2 world, cell;
    vec4 color = sampleCell(pixel, world, cell);
    if (color.a == 0.0) {
        result = vec4(0);
        if (!pixelOn || !outlineOn) return;
        vec2 dx = (toPixel * vec3(grid, 0, 0)).xy, dy = (toPixel * vec3(0, grid, 0)).xy;
        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
            vec2 position = vec2(pixel) + dx * float(x) + dy * float(y);
            ivec2 neighbor = ivec2(sign(position) * floor(abs(position) + 0.5));
            if (any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, textureSize(sourceImage, 0)))) continue;
            vec2 unusedWorld, unusedCell;
            if (sampleCell(neighbor, unusedWorld, unusedCell).a > 0.0) {
                result = vec4(quantizeOn ? nearestColor(outlineColor) : outlineColor, 1);
                return;
            }
        }
        return;
    }
    if (lightOn) {
        float light = max(0.0, 1.0 - length(world - lightPosition) / radius);
        color.rgb = clamp(color.rgb * (ambient + light * light * intensity * lightColor), 0.0, 1.0);
    }
    if (pixelOn && quantizeOn) {
        vec3 rgb = floor(color.rgb * 255.0 + 0.5) / 255.0;
        float first = 1e9, second = 1e9;
        int a = 0, b = 0;
        for (int i = 0; i < colorCount; ++i) {
            vec3 delta = rgb - palette[i]; float d = dot(delta, delta);
            if (d < first) { second = first; b = a; first = d; a = i; }
            else if (d < second) { second = d; b = i; }
        }
        vec3 direction = palette[b] - palette[a];
        float norm = dot(direction, direction);
        float ratio = norm > 0.0 ? clamp(dot(rgb - palette[a], direction) / norm, 0.0, 1.0) : 0.0;
        ivec2 pattern = ivec2(mod(cell, 4.0));
        color.rgb = palette[ditherOn && ratio > (float(bayer[pattern.y * 4 + pattern.x]) + 0.5) / 16.0 ? b : a];
    }
    result = color;
}
)GLSL";

GLuint compile(GLenum type, const char* source, std::string& error) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        error = log; glDeleteShader(shader); return 0;
    }
    return shader;
}

// Offscreen processing must not change the editor or ImGui renderer's GL state.
struct SavedState {
    GLint program, vao, drawFbo, readFbo, activeTexture, texture, sampler, unpack, unpackBuffer, rowLength, skipRows, skipPixels, viewport[4], polygon[2];
    GLboolean blend, scissor, depth, stencil, cull, srgb, dither, discard, mask[4];
    SavedState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture); glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture); glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack); glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows); glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
        glGetIntegerv(GL_POLYGON_MODE, polygon); glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        blend = glIsEnabled(GL_BLEND); scissor = glIsEnabled(GL_SCISSOR_TEST); depth = glIsEnabled(GL_DEPTH_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST); cull = glIsEnabled(GL_CULL_FACE); srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        dither = glIsEnabled(GL_DITHER); discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    }
    ~SavedState() {
        glUseProgram(program); glBindVertexArray(vao);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glBindTexture(GL_TEXTURE_2D, texture); glBindSampler(0, sampler); glActiveTexture(activeTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack); glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer); glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows); glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        glPolygonMode(GL_FRONT_AND_BACK, polygon[0]); glColorMask(mask[0],mask[1],mask[2],mask[3]);
        restore(GL_BLEND,blend); restore(GL_SCISSOR_TEST,scissor); restore(GL_DEPTH_TEST,depth);
        restore(GL_STENCIL_TEST,stencil); restore(GL_CULL_FACE,cull); restore(GL_FRAMEBUFFER_SRGB,srgb);
        restore(GL_DITHER,dither); restore(GL_RASTERIZER_DISCARD,discard);
    }
    static void restore(GLenum cap, GLboolean enabled) { if (enabled) glEnable(cap); else glDisable(cap); }
};
void matrix(GLint location, const rig::Affine& m) {
    const GLfloat data[] = {m.a,m.c,0,m.b,m.d,0,m.tx,m.ty,1};
    glUniformMatrix3fv(location, 1, GL_FALSE, data);
}
} // namespace

GpuEffects::~GpuEffects() { release(); }
void GpuEffects::release() {
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    if (source_) glDeleteTextures(1, &source_);
    if (output_) glDeleteTextures(1, &output_);
    program_ = vao_ = framebuffer_ = source_ = output_ = 0; width_ = height_ = 0;
}
bool GpuEffects::render(const std::vector<unsigned char>& rgba, int w, int h, const rig::Affine& toPixel,
                        const rig::RenderEffects& e, std::string& error) {
    error.clear();
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || rgba.size() != size_t(w) * h * 4 ||
        !rig::validRenderEffects(e) || !std::isfinite(toPixel.det()) || std::abs(toPixel.det()) < 1e-12f ||
        !std::isfinite(toPixel.tx) || !std::isfinite(toPixel.ty)) { error = "Invalid GPU preview input"; return false; }
    if (!glfwGetCurrentContext() || (!glad_glCreateShader && !gladLoadGL(glfwGetProcAddress)) || !GLAD_GL_VERSION_3_3) {
        error = "OpenGL 3.3 shader functions unavailable"; return false;
    }
    SavedState saved;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,0); glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    if (!program_) {
        GLuint vs = compile(GL_VERTEX_SHADER, vertex, error);
        if (!vs) return false;
        GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, error);
        if (!fs) { glDeleteShader(vs); return false; }
        program_ = glCreateProgram(); glAttachShader(program_, vs); glAttachShader(program_, fs); glLinkProgram(program_);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint ok; glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048] = {}; glGetProgramInfoLog(program_, sizeof(log), nullptr, log); error = log; release(); return false;
        }
        glGenVertexArrays(1, &vao_); glGenFramebuffers(1, &framebuffer_);
        glGenTextures(1, &source_); glGenTextures(1, &output_);
    }
    glBindSampler(0, 0);
    for (GLuint texture : {source_, output_}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, e.pixel.enabled ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, e.pixel.enabled ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (width_ != w || height_ != h) glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, source_); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output_,0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { error = "GPU preview framebuffer incomplete"; release(); return false; }
    glUseProgram(program_); glBindVertexArray(vao_); glViewport(0,0,w,h);
    for (auto cap : {GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_CULL_FACE,GL_FRAMEBUFFER_SRGB,GL_DITHER,GL_RASTERIZER_DISCARD}) glDisable(cap);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    auto uniform = [&](const char* name) { return glGetUniformLocation(program_,name); };
    glUniform1i(uniform("sourceImage"),0); matrix(uniform("toPixel"),toPixel); matrix(uniform("toWorld"),toPixel.inverse());
    glUniform1i(uniform("lightOn"),e.light.enabled); glUniform2f(uniform("lightPosition"),e.light.position.x,e.light.position.y);
    glUniform3fv(uniform("lightColor"),1,e.light.color.data());
    glUniform1f(uniform("radius"),e.light.radius); glUniform1f(uniform("intensity"),e.light.intensity); glUniform1f(uniform("ambient"),e.light.ambient);
    glUniform1i(uniform("pixelOn"),e.pixel.enabled); glUniform1f(uniform("grid"),(float)e.pixel.size);
    glUniform1i(uniform("quantizeOn"),e.pixel.quantize); glUniform1i(uniform("ditherOn"),e.pixel.dither);
    glUniform1i(uniform("outlineOn"),e.pixel.outline); glUniform3fv(uniform("outlineColor"),1,e.pixel.outlineColor.data());
    glUniform1i(uniform("colorCount"),(GLint)e.pixel.palette.size()); glUniform3fv(uniform("palette"),(GLsizei)e.pixel.palette.size(),e.pixel.palette[0].data());
    glDrawArrays(GL_TRIANGLES,0,3);
    if (glGetError() != GL_NO_ERROR) { error = "GPU preview rendering failed"; release(); return false; }
    width_ = w; height_ = h;
    return true;
}
