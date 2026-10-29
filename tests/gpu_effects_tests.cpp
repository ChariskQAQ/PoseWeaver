#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "platform/gpu_effects.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (false)

static std::vector<unsigned char> fixture(int w, int h) {
    std::vector<unsigned char> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t i = (size_t(y) * w + x) * 4;
        rgba[i] = (unsigned char)((x * 53 + y * 3) % 256);
        rgba[i+1] = (unsigned char)((x * 7 + y * 29) % 256);
        rgba[i+2] = (unsigned char)((x * 11 + y * 13) % 256);
        rgba[i+3] = x < 4 || y < 3 || x >= w - 5 || y >= h - 4 ? 0 : (unsigned char)((x * 31 + y * 17) % 256);
    }
    return rgba;
}
static void compare(GpuEffects& gpu, int w, int h, const rig::Affine& matrix, const rig::RenderEffects& effects) {
    auto source = fixture(w,h), expected = source;
    rig::applyRenderEffects(expected,w,h,matrix,effects);
    std::string error;
    if (!gpu.render(source,w,h,matrix,effects,error)) { std::printf("Shader failed: %s\n", error.c_str()); ++failures; return; }
    glBindTexture(GL_TEXTURE_2D,gpu.texture());
    std::vector<unsigned char> actual(expected.size());
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
    size_t mismatches = 0;
    for (size_t i = 0; i < actual.size(); ++i)
        if (std::abs((int)actual[i] - expected[i]) > (i % 4 == 3 ? 0 : 1)) ++mismatches;
    if (mismatches) {
        std::printf("Mismatch: %zu channels (%dx%d, light=%d pixel=%d quantize=%d dither=%d outline=%d colors=%zu)\n",
            mismatches,w,h,effects.light.enabled,effects.pixel.enabled,effects.pixel.quantize,effects.pixel.dither,effects.pixel.outline,effects.pixel.palette.size());
        ++failures;
    }
    CHECK(glGetError() == GL_NO_ERROR);
}
int main(int argc, char**) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    auto* window = glfwCreateWindow(64,64,"GPU effects regression",nullptr,nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    if (!gladLoadGL(glfwGetProcAddress)) return 1;
    std::printf("GPU effects device: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    {
        GpuEffects gpu;
        for (int mode = 0; mode < 32; ++mode) for (int size : {1,3,8}) {
            rig::RenderEffects effects;
            effects.light.enabled = (mode & 1) != 0; effects.pixel.enabled = (mode & 2) != 0;
            effects.pixel.quantize = (mode & 4) != 0; effects.pixel.dither = (mode & 8) != 0;
            effects.pixel.outline = (mode & 16) != 0; effects.pixel.size = size;
            effects.light.position = {-13,21}; effects.light.radius = 62; effects.light.ambient = 0.37f; effects.light.intensity = 2;
            compare(gpu,47,39,{1,0,0,-1,23.25f,11.75f},effects);
        }
        rig::RenderEffects effects;
        effects.light.enabled = effects.pixel.enabled = effects.pixel.dither = effects.pixel.outline = true;
        effects.pixel.size = 3;
        for (const rig::Affine matrix : {rig::Affine{0.5f,0,0,-0.5f,7.5f,8.25f},rig::Affine{0.8f,0.6f,0.6f,-0.8f,-31,49}})
            compare(gpu,39,53,matrix,effects);
        effects.pixel.palette = {{{0.2f,0.5f,0.8f}}}; compare(gpu,13,17,{1,0,0,-1,5,9},effects);
        effects.pixel.palette.clear();
        for (int i = 0; i < 256; ++i) effects.pixel.palette.push_back({i/255.0f,((i*17)%256)/255.0f,((i*31)%256)/255.0f});
        compare(gpu,13,17,{1,0,0,-1,5,9},effects);
        auto pixels = fixture(19,23);
        std::string error;
        CHECK(!gpu.render(pixels,0,23,{},effects,error) && !error.empty());
        CHECK(!gpu.render(pixels,19,23,{0,0,0,0,0,0},effects,error));
        auto invalid = effects; invalid.light.intensity = std::numeric_limits<float>::quiet_NaN();
        CHECK(!gpu.render(pixels,19,23,{},invalid,error));

        // Non-default caller state must survive both allocating and reusing the target.
        GLuint pbo, sampler;
        glGenBuffers(1,&pbo); glBindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo); glBufferData(GL_PIXEL_UNPACK_BUFFER,128,nullptr,GL_STREAM_DRAW);
        glGenSamplers(1,&sampler); glBindSampler(0,sampler);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,7); glPixelStorei(GL_UNPACK_SKIP_ROWS,2); glPixelStorei(GL_UNPACK_SKIP_PIXELS,1);
        glEnable(GL_SCISSOR_TEST); glScissor(0,0,1,1); glEnable(GL_BLEND); glEnable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE); glPolygonMode(GL_FRONT_AND_BACK,GL_LINE); glViewport(3,4,17,18);
        glActiveTexture(GL_TEXTURE3);
        for (int i = 0; i < 2; ++i) {
            CHECK(gpu.render(pixels,19,23,{},effects,error));
            GLint value, viewport[4]; GLboolean mask[4];
            glGetIntegerv(GL_ACTIVE_TEXTURE,&value); CHECK(value == GL_TEXTURE3);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&value); CHECK(value == (GLint)pbo);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH,&value); CHECK(value == 7);
            glGetIntegerv(GL_UNPACK_SKIP_ROWS,&value); CHECK(value == 2);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&value); CHECK(value == 1);
            glGetIntegerv(GL_VIEWPORT,viewport); CHECK(viewport[0] == 3 && viewport[1] == 4 && viewport[2] == 17 && viewport[3] == 18);
            glGetBooleanv(GL_COLOR_WRITEMASK,mask); CHECK(!mask[0] && mask[1] && !mask[2] && mask[3]);
            CHECK(glIsEnabled(GL_SCISSOR_TEST) && glIsEnabled(GL_BLEND) && glIsEnabled(GL_FRAMEBUFFER_SRGB));
            glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_SAMPLER_BINDING,&value); CHECK(value == (GLint)sampler); glActiveTexture(GL_TEXTURE3);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0); glDeleteBuffers(1,&pbo); glBindSampler(0,0); glDeleteSamplers(1,&sampler);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,0); glPixelStorei(GL_UNPACK_SKIP_ROWS,0); glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glPolygonMode(GL_FRONT_AND_BACK,GL_FILL); glActiveTexture(GL_TEXTURE0);
        GLuint texture = gpu.texture(); gpu.release(); CHECK(!glIsTexture(texture));
        CHECK(gpu.render(pixels,19,23,{},effects,error));
        CHECK(glGetError() == GL_NO_ERROR);
        if (argc > 1) {
            using Clock = std::chrono::steady_clock;
            const auto source = fixture(512,512);
            const rig::Affine matrix{1,0,0,-1,256,256};
            std::puts("case,width,height,samples,cpu_effect_ms_median,gpu_upload_effect_finish_ms_median");
            for (int mode = 0; mode < 3; ++mode) {
                effects = {};
                effects.light.enabled = mode != 1;
                effects.pixel.enabled = effects.pixel.dither = effects.pixel.outline = mode != 0;
                std::vector<double> cpu, gpuTimes;
                for (int sample = -2; sample < 9; ++sample) {
                    auto image = source;
                    auto start = Clock::now(); rig::applyRenderEffects(image,512,512,matrix,effects);
                    double cpuMs = std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                    start = Clock::now(); CHECK(gpu.render(source,512,512,matrix,effects,error)); glFinish();
                    double gpuMs = std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                    if (sample >= 0) { cpu.push_back(cpuMs); gpuTimes.push_back(gpuMs); }
                }
                std::sort(cpu.begin(),cpu.end()); std::sort(gpuTimes.begin(),gpuTimes.end());
                std::printf("%s,512,512,9,%.4f,%.4f\n",mode == 0 ? "light" : mode == 1 ? "pixel16" : "light_pixel16",cpu[4],gpuTimes[4]);
            }
        }
    }
    glfwDestroyWindow(window); glfwTerminate();
    std::printf("GPU effects: %d failure(s)\n",failures);
    return failures ? 1 : 0;
}
