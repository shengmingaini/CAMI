#pragma once

/// TASK-035 · Renderer 共享数学类型。
///
/// Vec3 复用 client/core 的定义（mmo::client::Vec3），此处仅补充渲染所需的
/// Mat4 / Plane / Frustum / AABB。所有矩阵采用**行主序**（m[row][col]），
/// 乘法/投影/视锥体提取均基于此约定，保证内部自洽（单测会校验剔除正确性）。
///
/// 注意：自由向量函数（Subtract/Add/.../Distance）必须在 Mat4 之前声明，
/// 因为 Mat4::LookAt 在类内定义时就要调用它们。

#include "mmo/client/types.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace mmo { namespace client { namespace render {

using Vec3 = mmo::client::Vec3;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline Vec3 Subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 Add(const Vec3& a, const Vec3& b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 Scale(const Vec3& a, float s) {
    return Vec3{a.x * s, a.y * s, a.z * s};
}
inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}
inline float Length(const Vec3& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
inline Vec3 Normalize(const Vec3& a) {
    const float l = Length(a);
    if (l < 1e-8f) return Vec3{0, 0, 0};
    const float inv = 1.0f / l;
    return Vec3{a.x * inv, a.y * inv, a.z * inv};
}
inline float Distance(const Vec3& a, const Vec3& b) {
    return Length(Subtract(a, b));
}

struct Mat4 {
    float m[4][4] = {};

    static Mat4 Identity() {
        Mat4 r;
        r.m[0][0] = 1.0f; r.m[1][1] = 1.0f; r.m[2][2] = 1.0f; r.m[3][3] = 1.0f;
        return r;
    }

    /// 行主序：C = A * B，即 C[i][j] = Σ_k A[i][k] * B[k][j]。
    static Mat4 Multiply(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }

    /// 透视投影（右手系，视线沿 -Z，近/远平面在 +Z 轴正向距离）。
    static Mat4 Perspective(float fov_y_rad, float aspect, float near_z, float far_z) {
        Mat4 r;
        const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = (far_z + near_z) / (near_z - far_z);
        r.m[2][3] = (2.0f * far_z * near_z) / (near_z - far_z);
        r.m[3][2] = -1.0f;
        return r;
    }

    /// 正交投影（UI 层用）。
    static Mat4 Ortho(float l, float r, float b, float t, float n, float f) {
        Mat4 m = Identity();
        m.m[0][0] = 2.0f / (r - l);
        m.m[1][1] = 2.0f / (t - b);
        m.m[2][2] = 2.0f / (f - n);
        m.m[0][3] = -(r + l) / (r - l);
        m.m[1][3] = -(t + b) / (t - b);
        m.m[2][3] = -(f + n) / (f - n);
        return m;
    }

    /// 视图矩阵：从 eye 看向 center，up 为上方向（右手系）。
    static Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = Normalize(Subtract(center, eye));          // forward (-Z)
        Vec3 r = Normalize(Cross(f, up));                   // right
        Vec3 u = Cross(r, f);                               // true up
        Mat4 m = Identity();
        m.m[0][0] = r.x;  m.m[0][1] = r.y;  m.m[0][2] = r.z;  m.m[0][3] = -Dot(r, eye);
        m.m[1][0] = u.x;  m.m[1][1] = u.y;  m.m[1][2] = u.z;  m.m[1][3] = -Dot(u, eye);
        m.m[2][0] = -f.x; m.m[2][1] = -f.y; m.m[2][2] = -f.z; m.m[2][3] =  Dot(f, eye);
        return m;
    }
};

/// 平面：a*x + b*y + c*z + d = 0，法线为单位向量。
struct Plane {
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;

    /// 点到平面的有符号距离（正=在平面法线方向一侧）。
    float DistanceTo(const Vec3& p) const noexcept {
        return a * p.x + b * p.y + c * p.z + d;
    }
};

/// 视锥体：6 个平面（左/右/下/上/近/远），法线指向视锥内部。
struct Frustum {
    Plane planes[6];

    /// 球体是否被视锥包含/相交（外部返回 false）。
    bool IntersectsSphere(const Vec3& center, float radius) const noexcept {
        for (int i = 0; i < 6; ++i) {
            if (planes[i].DistanceTo(center) < -radius) return false;
        }
        return true;
    }
};

/// 由 ViewProj（行主序，View*Proj）用 Gribb–Hartmann 提取视锥平面。
/// 行 i 对应矩阵第 i 行（m[i][0..3]）。约定：平面法线指向视锥内部，
/// 点在所有平面正侧（distance >= 0）即可见。
inline Frustum ExtractFrustum(const Mat4& vp) {
    Frustum f;
    auto row = [&](int i) -> const float* { return vp.m[i]; };
    const float* r0 = row(0);
    const float* r1 = row(1);
    const float* r2 = row(2);
    const float* r3 = row(3);

    auto make = [](const float* a, const float* b, float sb) {
        Plane p;
        p.a = a[0] + sb * b[0];
        p.b = a[1] + sb * b[1];
        p.c = a[2] + sb * b[2];
        p.d = a[3] + sb * b[3];
        const float len = std::sqrt(p.a * p.a + p.b * p.b + p.c * p.c);
        if (len > 1e-8f) { p.a /= len; p.b /= len; p.c /= len; p.d /= len; }
        return p;
    };
    f.planes[0] = make(r3, r0,  1.0f);  // left
    f.planes[1] = make(r3, r0, -1.0f);  // right
    f.planes[2] = make(r3, r1,  1.0f);  // bottom
    f.planes[3] = make(r3, r1, -1.0f);  // top
    f.planes[4] = make(r3, r2,  1.0f);  // near
    f.planes[5] = make(r3, r2, -1.0f);  // far
    return f;
}

}}}  // namespace mmo::client::render
