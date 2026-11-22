// Undertow core: engine-independent simulation code (C++17, no exceptions, no RTTI, no Unreal headers).
// Units: meters, seconds, radians. Plane: x east, y north; angles = atan2(y, x). The Unreal layer converts.
// Names here avoid Unreal macros (check, PI, TEXT, ...) because this code is also compiled inside the game module.
#pragma once

#include <cmath>
#include <cstdint>

namespace ut {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float saturate(float v) { return clampf(v, 0.0f, 1.0f); }
inline float sqr(float v) { return v * v; }

// Wraps an angle to (-pi, pi].
inline float wrapAngle(float a) {
    a = std::fmod(a + kPi, kTwoPi);
    if (a <= 0.0f) a += kTwoPi;
    return a - kPi;
}

// Moves `current` toward `target` by at most `maxDelta`.
inline float approach(float current, float target, float maxDelta) {
    const float d = target - current;
    if (d > maxDelta) return current + maxDelta;
    if (d < -maxDelta) return current - maxDelta;
    return target;
}

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float inX, float inY) : x(inX), y(inY) {}

    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }

    float dot(Vec2 o) const { return x * o.x + y * o.y; }
    float cross(Vec2 o) const { return x * o.y - y * o.x; }
    float lengthSq() const { return x * x + y * y; }
    float length() const { return std::sqrt(lengthSq()); }
    // Unit vector, or (1, 0) for a zero vector so callers never divide by zero.
    Vec2 normalized() const {
        const float len = length();
        return len > 1e-6f ? Vec2{x / len, y / len} : Vec2{1.0f, 0.0f};
    }
    // 90 degrees to the right of this direction (clockwise), i.e. the kerb side when driving on the right.
    Vec2 right() const { return {y, -x}; }
    float angle() const { return std::atan2(y, x); }

    static Vec2 fromAngle(float a) { return {std::cos(a), std::sin(a)}; }
};

inline float distance(Vec2 a, Vec2 b) { return (a - b).length(); }
inline float distanceSq(Vec2 a, Vec2 b) { return (a - b).lengthSq(); }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

// Closest point on segment ab to p. `outT` is the 0..1 position along the segment.
inline Vec2 closestOnSegment(Vec2 a, Vec2 b, Vec2 p, float* outT = nullptr) {
    const Vec2 ab = b - a;
    const float lenSq = ab.lengthSq();
    float t = lenSq > 1e-9f ? (p - a).dot(ab) / lenSq : 0.0f;
    t = saturate(t);
    if (outT) *outT = t;
    return a + ab * t;
}

}  // namespace ut
