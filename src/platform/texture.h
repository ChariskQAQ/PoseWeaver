#pragma once
#include <string>
#include <vector>
#include <memory>

struct Texture {
    unsigned int id = 0;  // OpenGL texture name, 0 = not loaded
    unsigned int premultipliedId = 0;  // created only when a non-normal blend mode needs it
    int width = 0, height = 0;
    // Immutable pixels let background exports retain a snapshot without copying large textures.
    std::shared_ptr<const std::vector<unsigned char>> pixels;
    const std::vector<unsigned char>& rgba() const {
        static const std::vector<unsigned char> empty;
        return pixels ? *pixels : empty;
    }

    unsigned char alphaAt(float u, float v) const;
    unsigned int premultiplied();
};

// Loads an image file (UTF-8 path) and uploads it to OpenGL. Returns false on failure.
bool loadTexture(const std::string& path, Texture& out, std::string* error);
void releaseTexture(Texture& tex);
bool updateTexturePixels(Texture& texture, int width, int height, std::vector<unsigned char> rgba, bool nearest);
