#pragma once
#include <string>
#include <vector>

struct Texture {
    unsigned int id = 0;  // OpenGL texture name, 0 = not loaded
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;  // CPU copy, used for alpha picking and mesh trimming

    unsigned char alphaAt(float u, float v) const;
};

// Loads an image file (UTF-8 path) and uploads it to OpenGL. Returns false on failure.
bool loadTexture(const std::string& path, Texture& out, std::string* error);
void releaseTexture(Texture& tex);
