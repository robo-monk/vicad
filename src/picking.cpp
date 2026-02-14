#include "picking.h"

#include <algorithm>
#include <cmath>

namespace vicad_picking {

using vicad_app::Vec3;
using vicad_app::add;
using vicad_app::mul;
using vicad_app::dot;
using vicad_app::cross;
using vicad_app::sub;
using vicad_app::normalize;

Vec3 CameraRayDirection(int mouse_x, int mouse_y, const PickContext &ctx) {
    const float w = (float)(ctx.viewport_width > 0 ? ctx.viewport_width : 1);
    const float h = (float)(ctx.viewport_height > 0 ? ctx.viewport_height : 1);
    const float nx = ((float)mouse_x / w) * 2.0f - 1.0f;
    const float ny = 1.0f - ((float)mouse_y / h) * 2.0f;
    const float tan_half = std::tan((ctx.fov_degrees * 3.1415926535f / 180.0f) * 0.5f);
    const float x_cam = nx * tan_half * (w / h);
    const float y_cam = ny * tan_half;
    Vec3 dir = add(ctx.basis.forward, add(mul(ctx.basis.right, x_cam), mul(ctx.basis.up, y_cam)));
    return normalize(dir);
}

void WindowMouseToPixel(int mouse_x, int mouse_y,
                        int window_w, int window_h,
                        int pixel_w, int pixel_h,
                        int *out_px, int *out_py) {
    if (!out_px || !out_py) return;
    const int ww = window_w > 0 ? window_w : 1;
    const int wh = window_h > 0 ? window_h : 1;
    const int pw = pixel_w > 0 ? pixel_w : 1;
    const int ph = pixel_h > 0 ? pixel_h : 1;
    const double sx = (double)pw / (double)ww;
    const double sy = (double)ph / (double)wh;
    int px = (int)std::lround((double)mouse_x * sx);
    int py = (int)std::lround((double)mouse_y * sy);
    px = std::clamp(px, 0, pw - 1);
    py = std::clamp(py, 0, ph - 1);
    *out_px = px;
    *out_py = py;
}

static bool ray_intersect_triangle(const Vec3 &orig, const Vec3 &dir,
                                   const Vec3 &v0, const Vec3 &v1, const Vec3 &v2,
                                   double *t_out) {
    const double eps = 1e-9;
    const Vec3 e1 = sub(v1, v0);
    const Vec3 e2 = sub(v2, v0);
    const Vec3 pvec = cross(dir, e2);
    const double det = (double)dot(e1, pvec);
    if (det > -eps && det < eps) return false;
    const double inv_det = 1.0 / det;
    const Vec3 tvec = sub(orig, v0);
    const double u = (double)dot(tvec, pvec) * inv_det;
    if (u < 0.0 || u > 1.0) return false;
    const Vec3 qvec = cross(tvec, e1);
    const double v = (double)dot(dir, qvec) * inv_det;
    if (v < 0.0 || u + v > 1.0) return false;
    const double t = (double)dot(e2, qvec) * inv_det;
    if (t <= eps) return false;
    if (t_out) *t_out = t;
    return true;
}

static bool ray_mesh_hit_t(const manifold::MeshGL &mesh,
                           const Vec3 &orig, const Vec3 &dir,
                           double *closest_t) {
    if (mesh.NumTri() == 0 || mesh.numProp < 3) return false;
    double best_t = 1e300;
    bool hit = false;
    for (size_t tri = 0; tri < mesh.NumTri(); ++tri) {
        const size_t i0 = mesh.triVerts[tri * 3 + 0];
        const size_t i1 = mesh.triVerts[tri * 3 + 1];
        const size_t i2 = mesh.triVerts[tri * 3 + 2];
        const Vec3 v0 = {
            mesh.vertProperties[i0 * mesh.numProp + 0],
            mesh.vertProperties[i0 * mesh.numProp + 1],
            mesh.vertProperties[i0 * mesh.numProp + 2],
        };
        const Vec3 v1 = {
            mesh.vertProperties[i1 * mesh.numProp + 0],
            mesh.vertProperties[i1 * mesh.numProp + 1],
            mesh.vertProperties[i1 * mesh.numProp + 2],
        };
        const Vec3 v2 = {
            mesh.vertProperties[i2 * mesh.numProp + 0],
            mesh.vertProperties[i2 * mesh.numProp + 1],
            mesh.vertProperties[i2 * mesh.numProp + 2],
        };
        double t = 0.0;
        if (ray_intersect_triangle(orig, dir, v0, v1, v2, &t) && t < best_t) {
            best_t = t;
            hit = true;
        }
    }
    if (hit && closest_t) *closest_t = best_t;
    return hit;
}

static bool ray_aabb_hit_t(const Vec3 &ray_origin, const Vec3 &ray_dir,
                           const Vec3 &bmin, const Vec3 &bmax,
                           double *t_out) {
    const double inf = 1e300;
    double tmin = -inf;
    double tmax = inf;
    const double o[3] = {ray_origin.x, ray_origin.y, ray_origin.z};
    const double d[3] = {ray_dir.x, ray_dir.y, ray_dir.z};
    const double mn[3] = {bmin.x, bmin.y, bmin.z};
    const double mx[3] = {bmax.x, bmax.y, bmax.z};

    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        const double inv = 1.0 / d[i];
        double t1 = (mn[i] - o[i]) * inv;
        double t2 = (mx[i] - o[i]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    if (tmax < 0.0) return false;
    const double t = tmin >= 0.0 ? tmin : tmax;
    if (t_out) *t_out = t;
    return true;
}

bool PickMeshHit(const manifold::MeshGL &mesh,
                 const Vec3 &eye,
                 const Vec3 &ray_dir) {
    return ray_mesh_hit_t(mesh, eye, ray_dir, nullptr);
}

int PickSceneObject(const std::vector<vicad::ScriptSceneObject> &scene,
                    const Vec3 &eye,
                    const Vec3 &ray_dir) {
    int best_index = -1;
    double best_t = 1e300;
    for (size_t i = 0; i < scene.size(); ++i) {
        const vicad::ScriptSceneObject &obj = scene[i];
        const Vec3 mn = {obj.bmin.x, obj.bmin.y, obj.bmin.z};
        const Vec3 mx = {obj.bmax.x, obj.bmax.y, obj.bmax.z};
        double t_box = 0.0;
        if (!ray_aabb_hit_t(eye, ray_dir, mn, mx, &t_box)) continue;

        double t_hit = t_box;
        if (obj.kind == vicad::ScriptSceneObjectKind::Manifold && obj.mesh.NumTri() > 0) {
            if (!ray_mesh_hit_t(obj.mesh, eye, ray_dir, &t_hit)) continue;
        }

        if (t_hit < best_t) {
            best_t = t_hit;
            best_index = (int)i;
        }
    }
    return best_index;
}

}  // namespace vicad_picking
