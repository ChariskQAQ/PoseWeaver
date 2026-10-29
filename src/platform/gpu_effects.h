#pragma once
#include "core/render_effects.h"

// Owns context-local resources. Render and release on the editor's OpenGL thread.
class GpuEffects {
public:
    ~GpuEffects();
    GpuEffects() = default;
    GpuEffects(const GpuEffects&) = delete;
    GpuEffects& operator=(const GpuEffects&) = delete;
    bool render(const std::vector<unsigned char>& rgba, int width, int height,
                const rig::Affine& worldToPixel, const rig::RenderEffects& effects, std::string& error);
    unsigned int texture() const { return output_; }
    void release();
private:
    unsigned int program_ = 0, vao_ = 0, framebuffer_ = 0, source_ = 0, output_ = 0;
    int width_ = 0, height_ = 0;
};
