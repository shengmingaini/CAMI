#pragma once

/// TASK-035 · Camera（透视 / 正交 / 视锥剔除）。
///
/// 设计要点：
///   - 相机持视图矩阵（LookAt）+ 投影矩阵（透视或正交），ViewProj = View * Proj。
///   - GetFrustum() 用 Gribb–Hartmann 从 ViewProj 提取 6 平面（法线指向内部）。
///   - IsVisible(center, radius)：球体视锥相交测试，渲染线程据此剔除。
///   - 不依赖任何服务端模块；纯数学，便于单测。

#include "mmo/client/render/math_types.h"

namespace mmo { namespace client { namespace render {

class Camera {
public:
    /// 透视投影（fov 单位为「度」）。
    void SetPerspective(float fov_degrees, float aspect, float near_z, float far_z) {
        proj_type_ = ProjType::Perspective;
        fov_ = fov_degrees; aspect_ = aspect; near_z_ = near_z; far_z_ = far_z;
        Rebuild();
    }

    /// 正交投影（UI / 2D 层用）。
    void SetOrtho(float left, float right, float bottom, float top,
                  float near_z, float far_z) {
        proj_type_ = ProjType::Ortho;
        o_l_ = left; o_r_ = right; o_b_ = bottom; o_t_ = top;
        near_z_ = near_z; far_z_ = far_z;
        Rebuild();
    }

    /// 设置视点：从 eye 看向 target。
    void LookAt(const Vec3& eye, const Vec3& target, const Vec3& up = Vec3{0.0f, 1.0f, 0.0f}) {
        eye_ = eye; target_ = target; up_ = up;
        Rebuild();
    }

    Mat4 ViewProj() const noexcept { return vp_; }
    Frustum GetFrustum() const noexcept { return frustum_; }
    const Vec3& Eye() const noexcept { return eye_; }

    /// 球体可见性（视锥相交）。
    bool IsVisible(const Vec3& center, float radius) const noexcept {
        return frustum_.IntersectsSphere(center, radius);
    }

private:
    enum class ProjType { Perspective, Ortho };

    void Rebuild() {
        if (proj_type_ == ProjType::Perspective) {
            const float fov_rad = fov_ * 3.14159265358979323846f / 180.0f;
            proj_ = Mat4::Perspective(fov_rad, aspect_, near_z_, far_z_);
        } else {
            proj_ = Mat4::Ortho(o_l_, o_r_, o_b_, o_t_, near_z_, far_z_);
        }
        view_ = Mat4::LookAt(eye_, target_, up_);
        vp_ = Mat4::Multiply(view_, proj_);
        frustum_ = ExtractFrustum(vp_);
    }

    ProjType proj_type_ = ProjType::Perspective;
    float fov_ = 60.0f, aspect_ = 16.0f / 9.0f, near_z_ = 0.1f, far_z_ = 1000.0f;
    float o_l_ = -1.0f, o_r_ = 1.0f, o_b_ = -1.0f, o_t_ = 1.0f;
    Vec3 eye_{0, 0, 0}, target_{0, 0, -1}, up_{0, 1, 0};
    Vec3 forward_{0.0f, 0.0f, -1.0f};
    Mat4 view_ = Mat4::Identity();
    Mat4 proj_ = Mat4::Identity();
    Mat4 vp_ = Mat4::Identity();
    Frustum frustum_{};
};

}}}  // namespace mmo::client::render
