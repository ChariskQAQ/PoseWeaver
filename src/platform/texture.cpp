#include "platform/texture.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>

#include <GLFW/glfw3.h>

#include <algorithm>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

unsigned char Texture::alphaAt(float u, float v) const {
    if (rgba.empty() || width <= 0 || height <= 0) return 255;
    int x = std::clamp((int)(u * width), 0, width - 1);
    int y = std::clamp((int)(v * height), 0, height - 1);
    return rgba[(size_t(y) * width + x) * 4 + 3];
}

bool loadTexture(const std::string& path, Texture& out, std::string* error) {
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) {
        if (error) *error = stbi_failure_reason() ? stbi_failure_reason() : "unknown image error";
        return false;
    }
    releaseTexture(out);
    out.width = w;
    out.height = h;
    out.rgba.assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.rgba.data());
    out.id = id;
    return true;
}

void releaseTexture(Texture& tex) {
    if (tex.id) {
        GLuint id = tex.id;
        glDeleteTextures(1, &id);
    }
    tex = Texture{};
}
