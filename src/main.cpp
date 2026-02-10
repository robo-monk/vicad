#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#define RGFW_OPENGL
#define RGFW_IMPLEMENTATION
#include "../RGFW.h"
#include "manifold/manifold.h"

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

struct Vec3 {
    float x;
    float y;
    float z;
};

struct CameraBasis {
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};

static Vec3 add(const Vec3 &a, const Vec3 &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3 mul(const Vec3 &v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

static Vec3 sub(const Vec3 &a, const Vec3 &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static Vec3 normalize(const Vec3 &v) {
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= 1e-8f) return {0.0f, 0.0f, 1.0f};

    const float inv_len = 1.0f / std::sqrt(len2);
    return {v.x * inv_len, v.y * inv_len, v.z * inv_len};
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static CameraBasis camera_basis(const Vec3 &eye, const Vec3 &target) {
    CameraBasis basis = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    basis.forward = normalize(sub(target, eye));
    basis.right = normalize(cross(basis.forward, {0.0f, 1.0f, 0.0f}));
    const float right_len2 = basis.right.x * basis.right.x + basis.right.y * basis.right.y +
                             basis.right.z * basis.right.z;
    if (right_len2 <= 1e-8f) {
        basis.right = {1.0f, 0.0f, 0.0f};
    }
    basis.up = normalize(cross(basis.right, basis.forward));
    return basis;
}

static void set_perspective(float fov_degrees, float aspect, float z_near, float z_far) {
    const float fov_radians = fov_degrees * 3.1415926535f / 180.0f;
    const float top = std::tan(fov_radians * 0.5f) * z_near;
    const float right = top * aspect;
    glFrustum(-right, right, -top, top, z_near, z_far);
}

static void apply_view_rotation(const CameraBasis &basis) {
    const float view[16] = {
        basis.right.x, basis.up.x, -basis.forward.x, 0.0f,
        basis.right.y, basis.up.y, -basis.forward.y, 0.0f,
        basis.right.z, basis.up.z, -basis.forward.z, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    glMultMatrixf(view);
}

static void apply_look_at(const Vec3 &eye, const Vec3 &target) {
    const CameraBasis basis = camera_basis(eye, target);
    apply_view_rotation(basis);
    glTranslatef(-eye.x, -eye.y, -eye.z);
}

static Vec3 camera_position(const Vec3 &target, float yaw_deg, float pitch_deg, float distance) {
    const float yaw = yaw_deg * 3.1415926535f / 180.0f;
    const float pitch = pitch_deg * 3.1415926535f / 180.0f;
    const float cp = std::cos(pitch);
    const Vec3 offset = {
        distance * cp * std::sin(yaw),
        distance * std::sin(pitch),
        distance * cp * std::cos(yaw),
    };
    return add(target, offset);
}

static void draw_grid() {
    const int half_cells = 80;
    const float step = 1.0f;

    glDisable(GL_LIGHTING);
    glLineWidth(1.0f);
    glBegin(GL_LINES);
    for (int i = -half_cells; i <= half_cells; ++i) {
        const float k = (float)i * step;
        const bool major = (i % 5) == 0;

        if (i == 0) {
            glColor3f(0.86f, 0.30f, 0.30f);
        } else {
            const float c = major ? 0.30f : 0.20f;
            glColor3f(c, c, c);
        }
        glVertex3f(k, 0.0f, -half_cells * step);
        glVertex3f(k, 0.0f, half_cells * step);

        if (i == 0) {
            glColor3f(0.30f, 0.56f, 0.90f);
        } else {
            const float c = major ? 0.30f : 0.20f;
            glColor3f(c, c, c);
        }
        glVertex3f(-half_cells * step, 0.0f, k);
        glVertex3f(half_cells * step, 0.0f, k);
    }
    glEnd();
}

static void draw_mesh(const manifold::MeshGL &mesh) {
    glEnable(GL_LIGHTING);
    glColor3f(0.84f, 0.86f, 0.89f);
    glBegin(GL_TRIANGLES);
    for (size_t tri = 0; tri < mesh.NumTri(); ++tri) {
        const size_t i0 = mesh.triVerts[tri * 3 + 0];
        const size_t i1 = mesh.triVerts[tri * 3 + 1];
        const size_t i2 = mesh.triVerts[tri * 3 + 2];

        const Vec3 p0 = {
            mesh.vertProperties[i0 * mesh.numProp + 0],
            mesh.vertProperties[i0 * mesh.numProp + 1],
            mesh.vertProperties[i0 * mesh.numProp + 2],
        };
        const Vec3 p1 = {
            mesh.vertProperties[i1 * mesh.numProp + 0],
            mesh.vertProperties[i1 * mesh.numProp + 1],
            mesh.vertProperties[i1 * mesh.numProp + 2],
        };
        const Vec3 p2 = {
            mesh.vertProperties[i2 * mesh.numProp + 0],
            mesh.vertProperties[i2 * mesh.numProp + 1],
            mesh.vertProperties[i2 * mesh.numProp + 2],
        };

        const Vec3 n = normalize(cross(sub(p1, p0), sub(p2, p0)));
        glNormal3f(n.x, n.y, n.z);
        glVertex3f(p0.x, p0.y, p0.z);
        glVertex3f(p1.x, p1.y, p1.z);
        glVertex3f(p2.x, p2.y, p2.z);
    }
    glEnd();
}

static void draw_selected_outline(const manifold::MeshGL &mesh) {
    glDisable(GL_LIGHTING);
    glColor3f(1.0f, 0.92f, 0.18f);
    glLineWidth(3.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    glPushMatrix();
    glScalef(1.01f, 1.01f, 1.01f);
    glBegin(GL_TRIANGLES);
    for (size_t tri = 0; tri < mesh.NumTri(); ++tri) {
        const size_t i0 = mesh.triVerts[tri * 3 + 0];
        const size_t i1 = mesh.triVerts[tri * 3 + 1];
        const size_t i2 = mesh.triVerts[tri * 3 + 2];

        const Vec3 p0 = {
            mesh.vertProperties[i0 * mesh.numProp + 0],
            mesh.vertProperties[i0 * mesh.numProp + 1],
            mesh.vertProperties[i0 * mesh.numProp + 2],
        };
        const Vec3 p1 = {
            mesh.vertProperties[i1 * mesh.numProp + 0],
            mesh.vertProperties[i1 * mesh.numProp + 1],
            mesh.vertProperties[i1 * mesh.numProp + 2],
        };
        const Vec3 p2 = {
            mesh.vertProperties[i2 * mesh.numProp + 0],
            mesh.vertProperties[i2 * mesh.numProp + 1],
            mesh.vertProperties[i2 * mesh.numProp + 2],
        };
        glVertex3f(p0.x, p0.y, p0.z);
        glVertex3f(p1.x, p1.y, p1.z);
        glVertex3f(p2.x, p2.y, p2.z);
    }
    glEnd();
    glPopMatrix();

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

static Vec3 camera_ray_direction(int mouse_x, int mouse_y, i32 width, i32 height,
                                 float fov_degrees, const CameraBasis &basis) {
    const float aspect = (float)width / (float)(height > 0 ? height : 1);
    const float tan_half_fov = std::tan((fov_degrees * 3.1415926535f / 180.0f) * 0.5f);
    const float ndc_x = (2.0f * ((float)mouse_x + 0.5f) / (float)width) - 1.0f;
    const float ndc_y = 1.0f - (2.0f * ((float)mouse_y + 0.5f) / (float)height);

    Vec3 dir = basis.forward;
    dir = add(dir, mul(basis.right, ndc_x * aspect * tan_half_fov));
    dir = add(dir, mul(basis.up, ndc_y * tan_half_fov));
    return normalize(dir);
}

static void window_mouse_to_pixel(i32 mouse_x, i32 mouse_y,
                                  i32 window_w, i32 window_h,
                                  i32 pixel_w, i32 pixel_h,
                                  i32 *out_x, i32 *out_y) {
    if (window_w <= 0 || window_h <= 0) {
        *out_x = mouse_x;
        *out_y = mouse_y;
        return;
    }
    const float sx = (float)pixel_w / (float)window_w;
    const float sy = (float)pixel_h / (float)window_h;
    *out_x = (i32)std::lround((float)mouse_x * sx);
    *out_y = (i32)std::lround((float)mouse_y * sy);
}

static bool ray_hits_aabb(const Vec3 &ray_origin, const Vec3 &ray_dir,
                          const Vec3 &bmin, const Vec3 &bmax) {
    float tmin = 0.0f;
    float tmax = 1e30f;

    const float ro[3] = {ray_origin.x, ray_origin.y, ray_origin.z};
    const float rd[3] = {ray_dir.x, ray_dir.y, ray_dir.z};
    const float mn[3] = {bmin.x, bmin.y, bmin.z};
    const float mx[3] = {bmax.x, bmax.y, bmax.z};

    for (int i = 0; i < 3; ++i) {
        if (std::fabs(rd[i]) < 1e-6f) {
            if (ro[i] < mn[i] || ro[i] > mx[i]) return false;
            continue;
        }
        const float inv = 1.0f / rd[i];
        float t0 = (mn[i] - ro[i]) * inv;
        float t1 = (mx[i] - ro[i]) * inv;
        if (t0 > t1) {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmax < tmin) return false;
    }

    return tmax >= 0.0f;
}

static void draw_orientation_cube(const CameraBasis &basis, i32 width, i32 height) {
    int size = (width < height ? width : height) / 5;
    if (size < 96) size = 96;
    if (size > 180) size = 180;
    const int pad = 16;
    const int vx = width - size - pad;
    const int vy = height - size - pad;

    glEnable(GL_SCISSOR_TEST);
    glScissor(vx, vy, size, size);
    glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glViewport(vx, vy, size, size);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    set_perspective(35.0f, 1.0f, 0.1f, 30.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -4.0f);
    apply_view_rotation(basis);

    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);

    const float s = 0.72f;
    glBegin(GL_QUADS);
    glColor3f(0.74f, 0.77f, 0.80f);
    glVertex3f(-s, -s, s); glVertex3f(s, -s, s); glVertex3f(s, s, s); glVertex3f(-s, s, s);
    glColor3f(0.66f, 0.69f, 0.73f);
    glVertex3f(-s, -s, -s); glVertex3f(-s, s, -s); glVertex3f(s, s, -s); glVertex3f(s, -s, -s);
    glColor3f(0.78f, 0.73f, 0.73f);
    glVertex3f(s, -s, -s); glVertex3f(s, s, -s); glVertex3f(s, s, s); glVertex3f(s, -s, s);
    glColor3f(0.73f, 0.78f, 0.73f);
    glVertex3f(-s, -s, -s); glVertex3f(-s, -s, s); glVertex3f(-s, s, s); glVertex3f(-s, s, -s);
    glColor3f(0.72f, 0.75f, 0.80f);
    glVertex3f(-s, s, -s); glVertex3f(-s, s, s); glVertex3f(s, s, s); glVertex3f(s, s, -s);
    glColor3f(0.65f, 0.68f, 0.72f);
    glVertex3f(-s, -s, -s); glVertex3f(s, -s, -s); glVertex3f(s, -s, s); glVertex3f(-s, -s, s);
    glEnd();

    glLineWidth(1.2f);
    glColor3f(0.1f, 0.1f, 0.1f);
    glBegin(GL_LINES);
    const float e = s;
    glVertex3f(-e, -e, -e); glVertex3f(e, -e, -e);
    glVertex3f(-e, e, -e); glVertex3f(e, e, -e);
    glVertex3f(-e, -e, e); glVertex3f(e, -e, e);
    glVertex3f(-e, e, e); glVertex3f(e, e, e);
    glVertex3f(-e, -e, -e); glVertex3f(-e, e, -e);
    glVertex3f(e, -e, -e); glVertex3f(e, e, -e);
    glVertex3f(-e, -e, e); glVertex3f(-e, e, e);
    glVertex3f(e, -e, e); glVertex3f(e, e, e);
    glVertex3f(-e, -e, -e); glVertex3f(-e, -e, e);
    glVertex3f(e, -e, -e); glVertex3f(e, -e, e);
    glVertex3f(-e, e, -e); glVertex3f(-e, e, e);
    glVertex3f(e, e, -e); glVertex3f(e, e, e);
    glEnd();

    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor3f(0.90f, 0.27f, 0.27f); glVertex3f(0.0f, 0.0f, 0.0f); glVertex3f(1.6f, 0.0f, 0.0f);
    glColor3f(0.37f, 0.90f, 0.37f); glVertex3f(0.0f, 0.0f, 0.0f); glVertex3f(0.0f, 1.6f, 0.0f);
    glColor3f(0.34f, 0.58f, 0.95f); glVertex3f(0.0f, 0.0f, 0.0f); glVertex3f(0.0f, 0.0f, 1.6f);
    glEnd();

    glEnable(GL_CULL_FACE);
}

static std::string read_text_file(const char *path) {
    std::string out;
    FILE *f = std::fopen(path, "rb");
    if (!f) return out;

    char buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n > 0) out.append(buf, n);
        if (n < sizeof(buf)) break;
    }
    std::fclose(f);
    return out;
}

static long long file_mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + (long long)st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    return (long long)st.st_mtime * 1000000000LL;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + (long long)st.st_mtim.tv_nsec;
#endif
}

static bool compute_mesh_bounds(const manifold::MeshGL &mesh, Vec3 *bmin, Vec3 *bmax) {
    if (mesh.numProp < 3 || mesh.vertProperties.empty()) return false;
    const size_t count = mesh.vertProperties.size() / mesh.numProp;
    if (count == 0) return false;

    Vec3 mn = {
        mesh.vertProperties[0],
        mesh.vertProperties[1],
        mesh.vertProperties[2],
    };
    Vec3 mx = mn;

    for (size_t i = 1; i < count; ++i) {
        const float x = mesh.vertProperties[i * mesh.numProp + 0];
        const float y = mesh.vertProperties[i * mesh.numProp + 1];
        const float z = mesh.vertProperties[i * mesh.numProp + 2];
        if (x < mn.x) mn.x = x;
        if (y < mn.y) mn.y = y;
        if (z < mn.z) mn.z = z;
        if (x > mx.x) mx.x = x;
        if (y > mx.y) mx.y = y;
        if (z > mx.z) mx.z = z;
    }

    *bmin = mn;
    *bmax = mx;
    return true;
}

static bool load_mesh_binary(const char *path, manifold::MeshGL *mesh, Vec3 *bmin, Vec3 *bmax, std::string *error) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        *error = "Failed to open script output binary.";
        return false;
    }

    char magic[8] = {0};
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "VCADMSH1", 8) != 0) {
        std::fclose(f);
        *error = "Invalid script output header.";
        return false;
    }

    uint32_t numProp = 0;
    uint32_t vertLen = 0;
    uint32_t triLen = 0;
    if (std::fread(&numProp, 4, 1, f) != 1 ||
        std::fread(&vertLen, 4, 1, f) != 1 ||
        std::fread(&triLen, 4, 1, f) != 1) {
        std::fclose(f);
        *error = "Failed reading script output metadata.";
        return false;
    }

    if (numProp < 3 || vertLen == 0 || triLen == 0 || (vertLen % numProp) != 0 || (triLen % 3) != 0) {
        std::fclose(f);
        *error = "Script output mesh metadata is invalid.";
        return false;
    }

    std::vector<float> vertProperties;
    std::vector<uint32_t> triVerts;
    vertProperties.resize(vertLen);
    triVerts.resize(triLen);

    if (std::fread(vertProperties.data(), sizeof(float), vertLen, f) != vertLen ||
        std::fread(triVerts.data(), sizeof(uint32_t), triLen, f) != triLen) {
        std::fclose(f);
        *error = "Script output mesh payload is incomplete.";
        return false;
    }

    std::fclose(f);

    manifold::MeshGL next;
    next.numProp = numProp;
    next.vertProperties = std::move(vertProperties);
    next.triVerts = std::move(triVerts);

    Vec3 mn = {0.0f, 0.0f, 0.0f};
    Vec3 mx = {0.0f, 0.0f, 0.0f};
    if (!compute_mesh_bounds(next, &mn, &mx)) {
        *error = "Script output mesh has no valid bounds.";
        return false;
    }

    *mesh = std::move(next);
    *bmin = mn;
    *bmax = mx;
    return true;
}

static bool run_script_and_load_mesh(const char *script_path,
                                     const char *out_path,
                                     const char *log_path,
                                     manifold::MeshGL *mesh,
                                     Vec3 *bmin,
                                     Vec3 *bmax,
                                     std::string *error) {
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "bun script-runner.ts \"%s\" \"%s\" > \"%s\" 2>&1",
                  script_path, out_path, log_path);

    const int rc = std::system(cmd);
    if (rc != 0) {
        *error = read_text_file(log_path);
        if (error->empty()) *error = "Bun script execution failed.";
        return false;
    }

    return load_mesh_binary(out_path, mesh, bmin, bmax, error);
}

int main() {
    manifold::Manifold fallback = manifold::Manifold::Cube(manifold::vec3(1.0), true);
    manifold::MeshGL mesh = fallback.GetMeshGL();
    Vec3 mesh_bmin = {-0.5f, -0.5f, -0.5f};
    Vec3 mesh_bmax = {0.5f, 0.5f, 0.5f};
    (void)compute_mesh_bounds(mesh, &mesh_bmin, &mesh_bmax);

    const char *script_path = "myobject.vicad";
    const char *script_out_path = "build/script_result.bin";
    const char *script_log_path = "build/script_error.log";

    long long last_script_mtime = -1;
    std::string script_error;

    RGFW_window *win = RGFW_createWindow(
        "vicad",
        100,
        100,
        1200,
        800,
        (RGFW_windowFlags)(RGFW_windowCenter | RGFW_windowOpenGL));
    if (win == nullptr) return 1;

    RGFW_window_setExitKey(win, RGFW_escape);
    RGFW_window_makeCurrentContext_OpenGL(win);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
    RGFW_window_swapInterval_OpenGL(win, 1);

    const float light_ambient[4] = {0.20f, 0.20f, 0.22f, 1.0f};
    const float light_diffuse[4] = {0.88f, 0.90f, 0.95f, 1.0f};
    const float light_specular[4] = {0.95f, 0.95f, 0.95f, 1.0f};
    const float mat_specular[4] = {0.35f, 0.35f, 0.35f, 1.0f};
    glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, light_specular);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 28.0f);

    RGFW_event event;
    i32 width = 0;
    i32 height = 0;
    i32 window_w = 0;
    i32 window_h = 0;
    RGFW_window_getSizeInPixels(win, &width, &height);
    RGFW_window_getSize(win, &window_w, &window_h);

    const float fov_degrees = 65.0f;
    Vec3 target = {0.0f, 0.0f, 0.0f};
    float yaw_deg = 45.0f;
    float pitch_deg = 24.0f;
    float distance = 80.0f;
    bool object_selected = false;
    i32 last_mouse_x = 0;
    i32 last_mouse_y = 0;
    bool have_last_mouse = false;

    while (!RGFW_window_shouldClose(win)) {
        const long long mt = file_mtime_ns(script_path);
        if (mt >= 0 && mt != last_script_mtime) {
            last_script_mtime = mt;
            manifold::MeshGL next_mesh;
            Vec3 next_bmin = {0.0f, 0.0f, 0.0f};
            Vec3 next_bmax = {0.0f, 0.0f, 0.0f};
            std::string err;

            if (run_script_and_load_mesh(script_path, script_out_path, script_log_path,
                                         &next_mesh, &next_bmin, &next_bmax, &err)) {
                mesh = std::move(next_mesh);
                mesh_bmin = next_bmin;
                mesh_bmax = next_bmax;
                object_selected = false;
                script_error.clear();
                std::fprintf(stderr, "[vicad] script loaded: %s\n", script_path);
            } else {
                script_error = err;
                std::fprintf(stderr, "[vicad] script error:\n%s\n", script_error.c_str());
            }
        }

        while (RGFW_window_checkEvent(win, &event)) {
            if (event.type == RGFW_quit) break;
            if (event.type == RGFW_windowResized || event.type == RGFW_scaleUpdated) {
                RGFW_window_getSizeInPixels(win, &width, &height);
                RGFW_window_getSize(win, &window_w, &window_h);
            }
            if (event.type == RGFW_mouseScroll) {
                distance *= std::pow(0.9f, event.scroll.y);
                distance = clampf(distance, 0.25f, 2000.0f);
            }
            if (event.type == RGFW_mousePosChanged) {
                if (!have_last_mouse) {
                    last_mouse_x = event.mouse.x;
                    last_mouse_y = event.mouse.y;
                    have_last_mouse = true;
                }

                const i32 dx = event.mouse.x - last_mouse_x;
                const i32 dy = event.mouse.y - last_mouse_y;
                last_mouse_x = event.mouse.x;
                last_mouse_y = event.mouse.y;

                const bool shift_down = RGFW_window_isKeyDown(win, RGFW_shiftL) ||
                                        RGFW_window_isKeyDown(win, RGFW_shiftR);
                const bool alt_down = RGFW_window_isKeyDown(win, RGFW_altL) ||
                                      RGFW_window_isKeyDown(win, RGFW_altR);
                const bool orbit_down = RGFW_window_isMouseDown(win, RGFW_mouseMiddle) ||
                                        (alt_down && RGFW_window_isMouseDown(win, RGFW_mouseLeft));

                if (orbit_down && !shift_down) {
                    yaw_deg += (float)dx * 0.35f;
                    pitch_deg -= (float)dy * 0.35f;
                    pitch_deg = clampf(pitch_deg, -89.0f, 89.0f);
                } else if (RGFW_window_isMouseDown(win, RGFW_mouseMiddle) && shift_down) {
                    const Vec3 eye = camera_position(target, yaw_deg, pitch_deg, distance);
                    const CameraBasis basis = camera_basis(eye, target);

                    const float world_per_pixel =
                        (2.0f * distance * std::tan((fov_degrees * 3.1415926535f / 180.0f) * 0.5f)) /
                        (float)(height > 0 ? height : 1);

                    target = add(target, mul(basis.right, -(float)dx * world_per_pixel));
                    target = add(target, mul(basis.up, (float)dy * world_per_pixel));
                }
            }
            if (event.type == RGFW_mouseButtonPressed && event.button.value == RGFW_mouseLeft) {
                const bool alt_down = RGFW_window_isKeyDown(win, RGFW_altL) ||
                                      RGFW_window_isKeyDown(win, RGFW_altR);
                if (!alt_down) {
                    i32 mouse_x = 0;
                    i32 mouse_y = 0;
                    RGFW_window_getMouse(win, &mouse_x, &mouse_y);
                    i32 mouse_px_x = 0;
                    i32 mouse_px_y = 0;
                    window_mouse_to_pixel(mouse_x, mouse_y, window_w, window_h, width, height,
                                          &mouse_px_x, &mouse_px_y);

                    const Vec3 eye = camera_position(target, yaw_deg, pitch_deg, distance);
                    const CameraBasis basis = camera_basis(eye, target);
                    const Vec3 ray_dir =
                        camera_ray_direction(mouse_px_x, mouse_px_y, width, height, fov_degrees, basis);
                    object_selected = ray_hits_aabb(eye, ray_dir, mesh_bmin, mesh_bmax);
                }
            }
        }

        if (height <= 0) height = 1;
        glViewport(0, 0, width, height);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        set_perspective(fov_degrees, (float)width / (float)height, 0.1f, 5000.0f);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        const Vec3 eye = camera_position(target, yaw_deg, pitch_deg, distance);
        const CameraBasis basis = camera_basis(eye, target);
        apply_look_at(eye, target);

        const float light_pos[4] = {6.0f, 8.0f, 6.0f, 1.0f};
        glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

        draw_grid();
        draw_mesh(mesh);
        if (object_selected) draw_selected_outline(mesh);
        draw_orientation_cube(basis, width, height);

        RGFW_window_swapBuffers_OpenGL(win);
    }

    RGFW_window_close(win);
    return 0;
}
