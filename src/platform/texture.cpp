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
    if (rgba().empty() || width <= 0 || height <= 0) return 255;
    int x = std::clamp((int)(u * width), 0, width - 1);
    int y = std::clamp((int)(v * height), 0, height - 1);
    return rgba()[(size_t(y) * width + x) * 4 + 3];
}

static unsigned int uploadTexture(int width, int height, const unsigned char* pixels) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return id;
}

bool updateTexturePixels(Texture& texture, int width, int height, std::vector<unsigned char> rgba, bool nearest) {
    if (width <= 0 || height <= 0 || rgba.size() != size_t(width) * height * 4) return false;
    if (!texture.id) glGenTextures(1, &texture.id);
    glBindTexture(GL_TEXTURE_2D, texture.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (texture.width != width || texture.height != height)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (glGetError() != GL_NO_ERROR) { releaseTexture(texture); return false; }
    texture.width = width; texture.height = height; texture.pixels = std::make_shared<const std::vector<unsigned char>>(std::move(rgba));
    return true;
}

unsigned int Texture::premultiplied() {
    if (!premultipliedId && id) {
        std::vector<unsigned char> premultipliedPixels = rgba();
        for (size_t p = 0; p < premultipliedPixels.size(); p += 4)
            for (int c = 0; c < 3; ++c) premultipliedPixels[p + c] = (premultipliedPixels[p + c] * premultipliedPixels[p + 3] + 127) / 255;
        premultipliedId = uploadTexture(width, height, premultipliedPixels.data());
    }
    return premultipliedId;
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
    out.pixels = std::make_shared<const std::vector<unsigned char>>(data, data + size_t(w) * h * 4);
    stbi_image_free(data);

    out.id = uploadTexture(w, h, out.rgba().data());
    return true;
}

void releaseTexture(Texture& tex) {
    if (tex.id) {
        GLuint id = tex.id;
        glDeleteTextures(1, &id);
    }
    if (tex.premultipliedId) {
        GLuint id = tex.premultipliedId;
        glDeleteTextures(1, &id);
    }
    tex = Texture{};
}
