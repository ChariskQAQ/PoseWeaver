#pragma once
#include <cmath>

namespace rig {

constexpr float kPi = 3.14159265358979323846f;
inline float degToRad(float d) { return d * (kPi / 180.0f); }
inline float radToDeg(float r) { return r * (180.0f / kPi); }

// Wrap an angle in degrees into (-180, 180].
inline float wrapDeg(float d) {
    d = std::fmod(d, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d <= -180.0f) d += 360.0f;
    return d;
}

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
};
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline float length(Vec2 v) { return std::sqrt(dot(v, v)); }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

// Distance from point p to segment ab.
inline float distToSegment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    float len2 = dot(ab, ab);
    float t = len2 > 1e-8f ? dot(p - a, ab) / len2 : 0.0f;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return length(p - (a + ab * t));
}

// 2x3 affine matrix:  | a b tx |
//                     | c d ty |
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    static Affine fromTRS(float x, float y, float rotDeg, float sx, float sy) {
        float r = degToRad(rotDeg);
        float cs = std::cos(r), sn = std::sin(r);
        return {cs * sx, -sn * sy, sn * sx, cs * sy, x, y};
    }
    Affine operator*(const Affine& o) const {
        return {a * o.a + b * o.c, a * o.b + b * o.d,
                c * o.a + d * o.c, c * o.b + d * o.d,
                a * o.tx + b * o.ty + tx, c * o.tx + d * o.ty + ty};
    }
    Vec2 apply(Vec2 p) const { return {a * p.x + b * p.y + tx, c * p.x + d * p.y + ty}; }
    Vec2 applyVector(Vec2 v) const { return {a * v.x + b * v.y, c * v.x + d * v.y}; }
    float det() const { return a * d - b * c; }
    Affine inverse() const {
        float id = det();
        id = std::fabs(id) > 1e-12f ? 1.0f / id : 0.0f;
        Affine r{d * id, -b * id, -c * id, a * id, 0, 0};
        r.tx = -(r.a * tx + r.b * ty);
        r.ty = -(r.c * tx + r.d * ty);
        return r;
    }
    // World rotation of the local x axis, in degrees.
    float rotationDeg() const { return radToDeg(std::atan2(c, a)); }
    Vec2 origin() const { return {tx, ty}; }
};

} // namespace rig
