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
#include "face_detection.h"
#include "script_worker_client.h"
#include "manifold/manifold.h"
#include "manifold/cross_section.h"

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#define CLAY_IMPLEMENTATION
#include "../clay.h"

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Vec2 {
    float x;
    float y;
};

enum class SketchPlane {
    XY,
    XZ,
    YZ,
};

struct SketchBasis {
    Vec3 origin;
    Vec3 u;
    Vec3 v;
    Vec3 normal;
};

struct SketchState {
    bool active;
    SketchPlane plane;
    float planeOffset;
    float snapStep;
    bool snapEnabled;
    float extrudeDepth;
    bool closed;
    bool hasHover;
    Vec2 hoverUv;
    std::vector<Vec2> points;
    std::string lastError;
};

struct FaceSelectState {
    bool enabled;
    bool dirty;
    float angleThresholdDeg;
    int hoveredRegion;
    int selectedRegion;
    vicad::FaceDetectionResult faces;
};

struct CameraBasis {
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};

struct ClayUiState {
    Clay_Arena arena;
    Clay_Context *ctx;
    bool initialized;
    bool panelCollapsed;
    Clay_ElementData panelData;
    Clay_ElementData headerData;
};

static ClayUiState g_ui = {};

static Clay_Dimensions measure_text_mono(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    const float font = (float)(config ? config->fontSize : 16);
    const float width = (float)text.length * (font * 0.62f);
    return (Clay_Dimensions){width, font};
}

static void clay_error_handler(Clay_ErrorData errorData) {
    std::fprintf(stderr, "[clay] %.*s\n", errorData.errorText.length, errorData.errorText.chars);
}

static void clay_init(i32 width, i32 height) {
    if (g_ui.initialized) return;
    uint32_t cap = Clay_MinMemorySize();
    g_ui.arena = Clay_CreateArenaWithCapacityAndMemory(cap, std::malloc(cap));
    g_ui.ctx = Clay_Initialize(
        g_ui.arena,
        (Clay_Dimensions){(float)width, (float)height},
        (Clay_ErrorHandler){.errorHandlerFunction = clay_error_handler, .userData = NULL});
    Clay_SetCurrentContext(g_ui.ctx);
    Clay_SetMeasureTextFunction(measure_text_mono, NULL);
    g_ui.initialized = true;
}

static float clay_color_chan(float c) {
    float v = c / 255.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static void draw_text_from_slice(float x, float y, float scale, Clay_StringSlice s, float r, float g, float b);

static void clay_render_commands(Clay_RenderCommandArray cmds, i32 pixel_width, i32 pixel_height, float ui_scale) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, pixel_width, pixel_height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glScalef(ui_scale, ui_scale, 1.0f);

    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    for (int i = 0; i < cmds.length; ++i) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&cmds, i);
        if (!cmd) continue;

        const Clay_BoundingBox box = cmd->boundingBox;
        const float x0 = box.x;
        const float y0 = box.y;
        const float x1 = box.x + box.width;
        const float y1 = box.y + box.height;

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const Clay_Color c = cmd->renderData.rectangle.backgroundColor;
                glColor4f(clay_color_chan(c.r), clay_color_chan(c.g), clay_color_chan(c.b), clay_color_chan(c.a));
                glBegin(GL_QUADS);
                glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
                glEnd();
            } break;
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const Clay_Color c = cmd->renderData.border.color;
                const Clay_BorderWidth w = cmd->renderData.border.width;
                glColor4f(clay_color_chan(c.r), clay_color_chan(c.g), clay_color_chan(c.b), clay_color_chan(c.a));
                glBegin(GL_QUADS);
                if (w.left > 0) {
                    glVertex2f(x0, y0); glVertex2f(x0 + w.left, y0); glVertex2f(x0 + w.left, y1); glVertex2f(x0, y1);
                }
                if (w.right > 0) {
                    glVertex2f(x1 - w.right, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x1 - w.right, y1);
                }
                if (w.top > 0) {
                    glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y0 + w.top); glVertex2f(x0, y0 + w.top);
                }
                if (w.bottom > 0) {
                    glVertex2f(x0, y1 - w.bottom); glVertex2f(x1, y1 - w.bottom); glVertex2f(x1, y1); glVertex2f(x0, y1);
                }
                glEnd();
            } break;
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const Clay_TextRenderData t = cmd->renderData.text;
                float sf = (float)t.fontSize / 7.0f;
                if (sf < 1.0f) sf = 1.0f;
                if (sf > 8.0f) sf = 8.0f;
                draw_text_from_slice(
                    x0, y0 + sf,
                    sf, t.stringContents,
                    clay_color_chan(t.textColor.r),
                    clay_color_chan(t.textColor.g),
                    clay_color_chan(t.textColor.b));
            } break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                const int sx = (int)std::lround(box.x * ui_scale);
                const int sy = pixel_height - (int)std::lround((box.y + box.height) * ui_scale);
                const int sw = (int)std::lround(box.width * ui_scale);
                const int sh = (int)std::lround(box.height * ui_scale);
                glEnable(GL_SCISSOR_TEST);
                glScissor(sx, sy, sw, sh);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                glDisable(GL_SCISSOR_TEST);
                break;
            default:
                break;
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

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

static float dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec2 sub2(const Vec2 &a, const Vec2 &b) {
    return {a.x - b.x, a.y - b.y};
}

static float length2_2d(const Vec2 &v) {
    return v.x * v.x + v.y * v.y;
}

static Vec2 snap_vec2(const Vec2 &v, float step) {
    if (step <= 1e-6f) return v;
    return {
        (float)std::lround(v.x / step) * step,
        (float)std::lround(v.y / step) * step,
    };
}

static const char *sketch_plane_name(SketchPlane plane) {
    switch (plane) {
        case SketchPlane::XY: return "XY";
        case SketchPlane::XZ: return "XZ";
        case SketchPlane::YZ: return "YZ";
        default: return "UNKNOWN";
    }
}

static SketchBasis sketch_basis(SketchPlane plane, float plane_offset) {
    SketchBasis b = {};
    switch (plane) {
        case SketchPlane::XY:
            b.origin = {0.0f, 0.0f, plane_offset};
            b.u = {1.0f, 0.0f, 0.0f};
            b.v = {0.0f, 1.0f, 0.0f};
            b.normal = {0.0f, 0.0f, 1.0f};
            break;
        case SketchPlane::XZ:
            b.origin = {0.0f, plane_offset, 0.0f};
            b.u = {1.0f, 0.0f, 0.0f};
            b.v = {0.0f, 0.0f, -1.0f};
            b.normal = {0.0f, 1.0f, 0.0f};
            break;
        case SketchPlane::YZ:
            b.origin = {plane_offset, 0.0f, 0.0f};
            b.u = {0.0f, 1.0f, 0.0f};
            b.v = {0.0f, 0.0f, 1.0f};
            b.normal = {1.0f, 0.0f, 0.0f};
            break;
    }
    return b;
}

static Vec3 sketch_uv_to_world(const SketchBasis &basis, const Vec2 &uv) {
    Vec3 p = basis.origin;
    p = add(p, mul(basis.u, uv.x));
    p = add(p, mul(basis.v, uv.y));
    return p;
}

static Vec2 world_to_sketch_uv(const SketchBasis &basis, const Vec3 &p) {
    const Vec3 d = sub(p, basis.origin);
    return {dot(d, basis.u), dot(d, basis.v)};
}

static bool ray_intersect_plane(const Vec3 &ray_origin, const Vec3 &ray_dir,
                                const Vec3 &plane_origin, const Vec3 &plane_normal,
                                Vec3 *hit) {
    const float denom = dot(ray_dir, plane_normal);
    if (std::fabs(denom) < 1e-6f) return false;
    const float t = dot(sub(plane_origin, ray_origin), plane_normal) / denom;
    if (t < 0.0f) return false;
    *hit = add(ray_origin, mul(ray_dir, t));
    return true;
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

static void draw_sketch_plane_grid(const SketchState &sketch) {
    if (!sketch.active) return;

    const SketchBasis basis = sketch_basis(sketch.plane, sketch.planeOffset);
    const int half_cells = 60;
    const float step = sketch.snapStep > 1e-6f ? sketch.snapStep : 1.0f;
    const float span = (float)half_cells * step;

    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glLineWidth(1.0f);
    glBegin(GL_LINES);
    for (int i = -half_cells; i <= half_cells; ++i) {
        const float k = (float)i * step;
        const bool major = (i % 5) == 0;

        if (i == 0) {
            glColor3f(0.98f, 0.60f, 0.20f);
        } else {
            const float c = major ? 0.40f : 0.30f;
            glColor3f(c, c + 0.04f, c + 0.08f);
        }
        Vec3 a = add(add(basis.origin, mul(basis.u, k)), mul(basis.v, -span));
        Vec3 b = add(add(basis.origin, mul(basis.u, k)), mul(basis.v, span));
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);

        if (i == 0) {
            glColor3f(0.25f, 0.78f, 1.0f);
        } else {
            const float c = major ? 0.38f : 0.28f;
            glColor3f(c, c + 0.05f, c + 0.10f);
        }
        Vec3 c0 = add(add(basis.origin, mul(basis.v, k)), mul(basis.u, -span));
        Vec3 c1 = add(add(basis.origin, mul(basis.v, k)), mul(basis.u, span));
        glVertex3f(c0.x, c0.y, c0.z);
        glVertex3f(c1.x, c1.y, c1.z);
    }
    glEnd();
    glLineWidth(1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

static void draw_sketch_profile(const SketchState &sketch) {
    if (!sketch.active) return;

    const SketchBasis basis = sketch_basis(sketch.plane, sketch.planeOffset);

    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    if (!sketch.points.empty()) {
        glLineWidth(2.5f);
        glColor3f(1.0f, 0.86f, 0.28f);
        glBegin(GL_LINE_STRIP);
        for (const Vec2 &uv : sketch.points) {
            const Vec3 p = sketch_uv_to_world(basis, uv);
            glVertex3f(p.x, p.y, p.z);
        }
        if (sketch.closed) {
            const Vec3 p0 = sketch_uv_to_world(basis, sketch.points.front());
            glVertex3f(p0.x, p0.y, p0.z);
        }
        glEnd();
        glLineWidth(1.0f);
    }

    if (!sketch.closed && sketch.hasHover && !sketch.points.empty()) {
        const Vec3 last = sketch_uv_to_world(basis, sketch.points.back());
        const Vec3 hover = sketch_uv_to_world(basis, sketch.hoverUv);
        glColor3f(0.88f, 0.94f, 1.0f);
        glLineWidth(1.8f);
        glBegin(GL_LINES);
        glVertex3f(last.x, last.y, last.z);
        glVertex3f(hover.x, hover.y, hover.z);
        glEnd();
        glLineWidth(1.0f);
    }

    if (!sketch.points.empty()) {
        glPointSize(7.0f);
        glBegin(GL_POINTS);
        for (size_t i = 0; i < sketch.points.size(); ++i) {
            const Vec3 p = sketch_uv_to_world(basis, sketch.points[i]);
            if (i == 0 && !sketch.closed) {
                glColor3f(0.30f, 0.98f, 0.58f);
            } else {
                glColor3f(1.0f, 0.86f, 0.28f);
            }
            glVertex3f(p.x, p.y, p.z);
        }
        if (!sketch.closed && sketch.hasHover) {
            const Vec3 hp = sketch_uv_to_world(basis, sketch.hoverUv);
            glColor3f(0.78f, 0.90f, 1.0f);
            glVertex3f(hp.x, hp.y, hp.z);
        }
        glEnd();
        glPointSize(1.0f);
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

static void draw_mesh(const manifold::MeshGL &mesh) {
    glEnable(GL_LIGHTING);
    glColor3f(0.82f, 0.84f, 0.87f);
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

static void draw_mesh_edge_strokes(const manifold::MeshGL &mesh) {
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(1.1f);
    glColor4f(0.12f, 0.16f, 0.20f, 0.42f);

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

    glLineWidth(1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void draw_face_region_overlay(const manifold::MeshGL &mesh,
                                     const vicad::FaceDetectionResult &faces,
                                     int region,
                                     float r, float g, float b, float a) {
    if (region < 0) return;
    if ((size_t)region >= faces.regions.size()) return;

    const std::vector<uint32_t> &tris = faces.regions[(size_t)region];
    if (tris.empty()) return;

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glColor4f(r, g, b, a);

    glBegin(GL_TRIANGLES);
    for (const uint32_t tri : tris) {
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

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
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

static Vec3 light_anchor(const Vec3 &eye, const CameraBasis &basis,
                         float right, float up, float forward) {
    Vec3 p = eye;
    p = add(p, mul(basis.right, right));
    p = add(p, mul(basis.up, up));
    p = add(p, mul(basis.forward, forward));
    return p;
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

static float sketch_close_threshold(const SketchState &sketch) {
    const float snap_term = sketch.snapEnabled ? sketch.snapStep * 0.35f : 0.0f;
    const float t = snap_term > 0.28f ? snap_term : 0.28f;
    return t;
}

static bool mouse_to_sketch_uv(i32 mouse_x, i32 mouse_y,
                               i32 width, i32 height,
                               const Vec3 &eye,
                               const CameraBasis &camera,
                               float fov_degrees,
                               const SketchState &sketch,
                               Vec2 *out_uv,
                               Vec3 *out_world) {
    if (width <= 0 || height <= 0) return false;
    const SketchBasis basis = sketch_basis(sketch.plane, sketch.planeOffset);
    const Vec3 ray_dir = camera_ray_direction(mouse_x, mouse_y, width, height, fov_degrees, camera);
    Vec3 hit = {0.0f, 0.0f, 0.0f};
    if (!ray_intersect_plane(eye, ray_dir, basis.origin, basis.normal, &hit)) return false;
    Vec2 uv = world_to_sketch_uv(basis, hit);
    if (sketch.snapEnabled) uv = snap_vec2(uv, sketch.snapStep);
    if (out_uv) *out_uv = uv;
    if (out_world) *out_world = sketch_uv_to_world(basis, uv);
    return true;
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

static float ui_scale_for_window(i32 pixel_w, i32 pixel_h, i32 window_w, i32 window_h) {
    if (window_w <= 0 || window_h <= 0) return 1.0f;
    const float sx = (float)pixel_w / (float)window_w;
    const float sy = (float)pixel_h / (float)window_h;
    float s = sx < sy ? sx : sy;
    if (s < 1.0f) s = 1.0f;
    return s;
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

static void draw_orientation_cube(const CameraBasis &basis, i32 width, i32 height, int top_offset_px) {
    int size = (width < height ? width : height) / 5;
    if (size < 96) size = 96;
    if (size > 180) size = 180;
    const int pad = 16;
    const int vx = width - size - pad;
    const int vy = height - size - pad - top_offset_px;

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
    glViewport(0, 0, width, height);
}

struct StatusPanelRect {
    int x;
    int y;
    int w;
    int h;
    int header_h;
};

static StatusPanelRect status_panel_rect(i32 width, i32 height, bool collapsed) {
    (void)height;
    StatusPanelRect r = {0, 0, 0, 0, 0};
    r.w = 320;
    r.header_h = 28;
    r.h = collapsed ? r.header_h : 140;
    r.x = (int)width - r.w - 16;
    r.y = 16;
    return r;
}

static bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh);
}

static uint8_t glyph_rows(char ch, int row) {
    // 5x7 uppercase glyphs for the small status HUD.
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    switch (ch) {
        case 'A': { const uint8_t g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g[row]; }
        case 'B': { const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g[row]; }
        case 'C': { const uint8_t g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g[row]; }
        case 'D': { const uint8_t g[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return g[row]; }
        case 'E': { const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g[row]; }
        case 'F': { const uint8_t g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g[row]; }
        case 'H': { const uint8_t g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g[row]; }
        case 'I': { const uint8_t g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}; return g[row]; }
        case 'K': { const uint8_t g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g[row]; }
        case 'L': { const uint8_t g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g[row]; }
        case 'M': { const uint8_t g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g[row]; }
        case 'N': { const uint8_t g[7] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11}; return g[row]; }
        case 'O': { const uint8_t g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g[row]; }
        case 'P': { const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g[row]; }
        case 'R': { const uint8_t g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g[row]; }
        case 'S': { const uint8_t g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g[row]; }
        case 'T': { const uint8_t g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g[row]; }
        case 'U': { const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g[row]; }
        case 'V': { const uint8_t g[7] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}; return g[row]; }
        case 'X': { const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return g[row]; }
        case 'Y': { const uint8_t g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g[row]; }
        case '0': { const uint8_t g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g[row]; }
        case '1': { const uint8_t g[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return g[row]; }
        case '2': { const uint8_t g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return g[row]; }
        case '3': { const uint8_t g[7] = {0x1F,0x01,0x02,0x06,0x01,0x11,0x0E}; return g[row]; }
        case '4': { const uint8_t g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g[row]; }
        case '5': { const uint8_t g[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}; return g[row]; }
        case '6': { const uint8_t g[7] = {0x07,0x08,0x10,0x1E,0x11,0x11,0x0E}; return g[row]; }
        case '7': { const uint8_t g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g[row]; }
        case '8': { const uint8_t g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g[row]; }
        case '9': { const uint8_t g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x1C}; return g[row]; }
        case ':': { const uint8_t g[7] = {0x00,0x04,0x00,0x00,0x04,0x00,0x00}; return g[row]; }
        case '.': { const uint8_t g[7] = {0x00,0x00,0x00,0x00,0x00,0x06,0x06}; return g[row]; }
        case '-': { const uint8_t g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g[row]; }
        case '+': { const uint8_t g[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; return g[row]; }
        case '=': { const uint8_t g[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}; return g[row]; }
        case '/': { const uint8_t g[7] = {0x01,0x02,0x04,0x08,0x10,0x00,0x00}; return g[row]; }
        case '(': { const uint8_t g[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return g[row]; }
        case ')': { const uint8_t g[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return g[row]; }
        case '[': { const uint8_t g[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; return g[row]; }
        case ']': { const uint8_t g[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; return g[row]; }
        case ' ': return 0x00;
        default:  { const uint8_t g[7] = {0x1F,0x11,0x02,0x04,0x04,0x00,0x04}; return g[row]; }
    }
}

static void draw_text_5x7(float x, float y, float scale, const char *text, float r, float g, float b) {
    if (!text) return;
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    float cx = x;
    for (const char *p = text; *p; ++p) {
        const char ch = *p;
        for (int row = 0; row < 7; ++row) {
            const uint8_t bits = glyph_rows(ch, row);
            for (int col = 0; col < 5; ++col) {
                if ((bits >> (4 - col)) & 1) {
                    const float px = cx + col * scale;
                    const float py = y + row * scale;
                    glVertex2f(px, py);
                    glVertex2f(px + scale, py);
                    glVertex2f(px + scale, py + scale);
                    glVertex2f(px, py + scale);
                }
            }
        }
        cx += 6 * scale;
    }
    glEnd();
}

static void draw_text_from_slice(float x, float y, float scale, Clay_StringSlice s, float r, float g, float b) {
    if (s.length <= 0 || !s.chars) return;
    char tmp[512];
    int n = s.length;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    std::memcpy(tmp, s.chars, (size_t)n);
    tmp[n] = '\0';
    draw_text_5x7(x, y, scale, tmp, r, g, b);
}

static int draw_status_panel(i32 width, i32 height, bool collapsed,
                             bool script_ok, const std::string &script_error,
                             size_t tri_count, size_t vert_count, bool selected) {
    StatusPanelRect p = status_panel_rect(width, height, collapsed);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    glBegin(GL_QUADS);
    glColor4f(0.08f, 0.10f, 0.13f, 0.92f);
    glVertex2i(p.x, p.y); glVertex2i(p.x + p.w, p.y); glVertex2i(p.x + p.w, p.y + p.h); glVertex2i(p.x, p.y + p.h);
    glColor4f(0.14f, 0.17f, 0.21f, 0.98f);
    glVertex2i(p.x, p.y); glVertex2i(p.x + p.w, p.y); glVertex2i(p.x + p.w, p.y + p.header_h); glVertex2i(p.x, p.y + p.header_h);
    glEnd();

    glLineWidth(1.0f);
    glBegin(GL_LINE_LOOP);
    glColor3f(0.35f, 0.39f, 0.46f);
    glVertex2i(p.x, p.y); glVertex2i(p.x + p.w, p.y); glVertex2i(p.x + p.w, p.y + p.h); glVertex2i(p.x, p.y + p.h);
    glEnd();

    draw_text_5x7(p.x + 12, p.y + 8, 2, "STATUS", 0.95f, 0.97f, 0.99f);
    draw_text_5x7(p.x + p.w - 26, p.y + 8, 2, collapsed ? "+" : "-", 0.95f, 0.97f, 0.99f);

    if (!collapsed) {
        char line[128] = {0};
        std::snprintf(line, sizeof(line), "SCRIPT: %s", script_ok ? "OK" : "ERROR");
        draw_text_5x7(p.x + 12, p.y + 38, 2, line, script_ok ? 0.55f : 1.0f, script_ok ? 0.90f : 0.45f, 0.55f);

        std::snprintf(line, sizeof(line), "MESH: %zu TRI  %zu VERT", tri_count, vert_count);
        draw_text_5x7(p.x + 12, p.y + 56, 2, line, 0.84f, 0.88f, 0.93f);

        std::snprintf(line, sizeof(line), "SELECTED: %s", selected ? "ON" : "OFF");
        draw_text_5x7(p.x + 12, p.y + 74, 2, line, selected ? 1.0f : 0.75f, selected ? 0.92f : 0.78f, selected ? 0.18f : 0.80f);

        if (!script_ok) {
            const std::string one_line = script_error.empty() ? "NO ERROR TEXT" :
                script_error.substr(0, script_error.find('\n'));
            draw_text_5x7(p.x + 12, p.y + 96, 2, "LAST ERROR:", 1.0f, 0.75f, 0.75f);
            draw_text_5x7(p.x + 12, p.y + 114, 2, one_line.c_str(), 0.95f, 0.80f, 0.80f);
        }
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    return p.h + 8;
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

static const char *manifold_error_string(manifold::Manifold::Error error) {
    switch (error) {
        case manifold::Manifold::Error::NoError: return "No Error";
        case manifold::Manifold::Error::NonFiniteVertex: return "Non Finite Vertex";
        case manifold::Manifold::Error::NotManifold: return "Not Manifold";
        case manifold::Manifold::Error::VertexOutOfBounds: return "Vertex Out Of Bounds";
        case manifold::Manifold::Error::PropertiesWrongLength: return "Properties Wrong Length";
        case manifold::Manifold::Error::MissingPositionProperties: return "Missing Position Properties";
        case manifold::Manifold::Error::MergeVectorsDifferentLengths: return "Merge Vectors Different Lengths";
        case manifold::Manifold::Error::MergeIndexOutOfBounds: return "Merge Index Out Of Bounds";
        case manifold::Manifold::Error::TransformWrongLength: return "Transform Wrong Length";
        case manifold::Manifold::Error::RunIndexWrongLength: return "Run Index Wrong Length";
        case manifold::Manifold::Error::FaceIDWrongLength: return "Face ID Wrong Length";
        case manifold::Manifold::Error::InvalidConstruction: return "Invalid Construction";
        case manifold::Manifold::Error::ResultTooLarge: return "Result Too Large";
        default: return "Unknown Error";
    }
}

static bool build_sketch_feature(const SketchState &sketch,
                                 manifold::Manifold *feature,
                                 std::string *error) {
    if (!sketch.closed || sketch.points.size() < 3) {
        *error = "Sketch profile must be closed before finishing.";
        return false;
    }
    if (!(sketch.extrudeDepth > 1e-4f)) {
        *error = "Extrude depth must be positive.";
        return false;
    }

    manifold::SimplePolygon contour;
    contour.reserve(sketch.points.size());
    for (const Vec2 &p : sketch.points) {
        contour.push_back(manifold::vec2((double)p.x, (double)p.y));
    }

    manifold::CrossSection profile(contour, manifold::CrossSection::FillRule::Positive);
    profile = profile.Simplify(1e-6);
    if (profile.IsEmpty() || profile.NumContour() == 0 || std::fabs(profile.Area()) < 1e-9) {
        *error = "Sketch profile is invalid or area is too small.";
        return false;
    }
    manifold::Polygons polys = profile.ToPolygons();

    manifold::Manifold local = manifold::Manifold::Extrude(polys, (double)sketch.extrudeDepth);
    if (local.Status() != manifold::Manifold::Error::NoError) {
        *error = std::string("Sketch extrusion failed: ") + manifold_error_string(local.Status());
        return false;
    }

    const SketchBasis basis = sketch_basis(sketch.plane, sketch.planeOffset);
    const manifold::mat3x4 xf(
        manifold::vec3((double)basis.u.x, (double)basis.u.y, (double)basis.u.z),
        manifold::vec3((double)basis.v.x, (double)basis.v.y, (double)basis.v.z),
        manifold::vec3((double)basis.normal.x, (double)basis.normal.y, (double)basis.normal.z),
        manifold::vec3((double)basis.origin.x, (double)basis.origin.y, (double)basis.origin.z));

    manifold::Manifold placed = local.Transform(xf);
    if (placed.Status() != manifold::Manifold::Error::NoError) {
        *error = std::string("Sketch transform failed: ") + manifold_error_string(placed.Status());
        return false;
    }

    *feature = std::move(placed);
    return true;
}

static bool compose_scene_mesh(const manifold::MeshGL &base_mesh,
                               const std::vector<manifold::Manifold> &features,
                               manifold::MeshGL *mesh,
                               Vec3 *bmin,
                               Vec3 *bmax,
                               std::string *error) {
    if (features.empty()) {
        Vec3 mn = {0.0f, 0.0f, 0.0f};
        Vec3 mx = {0.0f, 0.0f, 0.0f};
        if (!compute_mesh_bounds(base_mesh, &mn, &mx)) {
            *error = "Base mesh has no valid bounds.";
            return false;
        }
        *mesh = base_mesh;
        *bmin = mn;
        *bmax = mx;
        return true;
    }

    manifold::Manifold scene(base_mesh);
    if (scene.Status() != manifold::Manifold::Error::NoError) {
        *error = std::string("Base mesh is not manifold: ") + manifold_error_string(scene.Status());
        return false;
    }

    for (const manifold::Manifold &feature : features) {
        scene += feature;
        if (scene.Status() != manifold::Manifold::Error::NoError) {
            *error = std::string("Feature union failed: ") + manifold_error_string(scene.Status());
            return false;
        }
    }

    manifold::MeshGL next = scene.GetMeshGL();
    Vec3 mn = {0.0f, 0.0f, 0.0f};
    Vec3 mx = {0.0f, 0.0f, 0.0f};
    if (!compute_mesh_bounds(next, &mn, &mx)) {
        *error = "Composed mesh has no valid bounds.";
        return false;
    }

    *mesh = std::move(next);
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

static Clay_RenderCommandArray build_clay_ui(i32 width, i32 height, bool script_ok,
                                             const std::string &script_error,
                                             size_t tri_count, size_t vert_count,
                                             bool selected,
                                             const SketchState &sketch,
                                             size_t sketch_feature_count,
                                             const FaceSelectState &face_select) {
    const float hud = 2.0f;
    const int root_pad = (int)std::lround(16.0f * hud);
    int panel_w = (int)std::lround(320.0f * hud);
    if (panel_w > width - root_pad * 2) panel_w = width - root_pad * 2;
    if (panel_w < 320) panel_w = 320;
    const int header_h = (int)std::lround(28.0f * hud);
    const int header_pad = (int)std::lround(8.0f * hud);
    const int body_pad = (int)std::lround(10.0f * hud);
    const int body_gap = (int)std::lround(6.0f * hud);
    const int title_font = (int)std::lround(16.0f * hud);
    const int body_font = (int)std::lround(14.0f * hud);
    const int small_font = (int)std::lround(13.0f * hud);

    clay_init(width, height);
    Clay_SetCurrentContext(g_ui.ctx);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
    Clay_BeginLayout();

    char mesh_line[96];
    char selected_line[96];
    char sketch_mode_line[96];
    char sketch_plane_line[96];
    char sketch_profile_line[96];
    char sketch_depth_line[96];
    char sketch_snap_line[96];
    char sketch_feature_line[96];
    char face_mode_line[96];
    char face_region_line[96];
    char face_hover_line[96];
    char face_type_line[128];
    std::snprintf(mesh_line, sizeof(mesh_line), "MESH: %zu TRI  %zu VERT", tri_count, vert_count);
    std::snprintf(selected_line, sizeof(selected_line), "SELECTED: %s", selected ? "ON" : "OFF");
    std::snprintf(sketch_mode_line, sizeof(sketch_mode_line), "SKETCH: %s [S]", sketch.active ? "ON" : "OFF");
    std::snprintf(sketch_plane_line, sizeof(sketch_plane_line), "PLANE: %s [1/2/3]", sketch_plane_name(sketch.plane));
    std::snprintf(sketch_profile_line, sizeof(sketch_profile_line), "PROFILE: %s  %zu PT",
                  sketch.closed ? "CLOSED" : "OPEN", sketch.points.size());
    std::snprintf(sketch_depth_line, sizeof(sketch_depth_line), "DEPTH: %.2f [+/-]", sketch.extrudeDepth);
    std::snprintf(sketch_snap_line, sizeof(sketch_snap_line), "SNAP: %s  %.2f [G]",
                  sketch.snapEnabled ? "ON" : "OFF", sketch.snapStep);
    std::snprintf(sketch_feature_line, sizeof(sketch_feature_line), "FEATURES: %zu", sketch_feature_count);
    std::snprintf(face_mode_line, sizeof(face_mode_line), "FACE SEL: %s [F]", face_select.enabled ? "ON" : "OFF");
    std::snprintf(face_region_line, sizeof(face_region_line), "FACE REGIONS: %zu  ANGLE: %.1f",
                  face_select.faces.regions.size(), face_select.angleThresholdDeg);
    std::snprintf(face_hover_line, sizeof(face_hover_line), "HOVER/SEL: %d / %d",
                  face_select.hoveredRegion, face_select.selectedRegion);
    const vicad::FacePrimitiveType hover_type =
        (face_select.hoveredRegion >= 0 &&
         (size_t)face_select.hoveredRegion < face_select.faces.regionType.size())
            ? face_select.faces.regionType[(size_t)face_select.hoveredRegion]
            : vicad::FacePrimitiveType::Unknown;
    const vicad::FacePrimitiveType sel_type =
        (face_select.selectedRegion >= 0 &&
         (size_t)face_select.selectedRegion < face_select.faces.regionType.size())
            ? face_select.faces.regionType[(size_t)face_select.selectedRegion]
            : vicad::FacePrimitiveType::Unknown;
    std::snprintf(face_type_line, sizeof(face_type_line), "TYPE H/S: %s / %s",
                  vicad::FacePrimitiveTypeName(hover_type),
                  vicad::FacePrimitiveTypeName(sel_type));

    const std::string one_line_error = script_error.empty() ? "NO ERROR TEXT" :
        script_error.substr(0, script_error.find('\n'));
    const std::string one_line_sketch_error = sketch.lastError.empty() ? "" :
        sketch.lastError.substr(0, sketch.lastError.find('\n'));

    Clay_String mesh_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(mesh_line), .chars = mesh_line};
    Clay_String selected_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(selected_line), .chars = selected_line};
    Clay_String sketch_mode_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(sketch_mode_line), .chars = sketch_mode_line};
    Clay_String sketch_plane_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(sketch_plane_line), .chars = sketch_plane_line};
    Clay_String sketch_profile_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(sketch_profile_line), .chars = sketch_profile_line};
    Clay_String sketch_depth_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(sketch_depth_line), .chars = sketch_depth_line};
    Clay_String sketch_snap_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(sketch_snap_line), .chars = sketch_snap_line};
    Clay_String sketch_feature_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(sketch_feature_line), .chars = sketch_feature_line};
    Clay_String face_mode_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(face_mode_line), .chars = face_mode_line};
    Clay_String face_region_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(face_region_line), .chars = face_region_line};
    Clay_String face_hover_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(face_hover_line), .chars = face_hover_line};
    Clay_String face_type_line_s = {.isStaticallyAllocated = false, .length = (int32_t)std::strlen(face_type_line), .chars = face_type_line};
    Clay_String err_line_s = {.isStaticallyAllocated = false, .length = (int32_t)one_line_error.size(), .chars = one_line_error.c_str()};
    Clay_String sketch_err_line_s = {.isStaticallyAllocated = false, .length = (int32_t)one_line_sketch_error.size(), .chars = one_line_sketch_error.c_str()};

    CLAY(CLAY_ID("StatusRoot"), {
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .padding = CLAY_PADDING_ALL((uint16_t)root_pad),
            .childAlignment = {.x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_TOP},
        }
    }) {
        CLAY(CLAY_ID("StatusPanel"), {
            .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = {.width = CLAY_SIZING_FIXED((float)panel_w), .height = CLAY_SIZING_FIT(0)},
            },
            .backgroundColor = {20, 25, 33, 235},
            .border = {.color = {90, 100, 118, 255}, .width = {.left = 1, .right = 1, .top = 1, .bottom = 1}}
        }) {
            CLAY(CLAY_ID("StatusHeader"), {
                .layout = {
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED((float)header_h)},
                    .padding = {(uint16_t)header_pad, (uint16_t)header_pad, (uint16_t)(header_pad * 3 / 4), (uint16_t)(header_pad * 3 / 4)},
                    .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = {36, 44, 54, 250},
            }) {
                CLAY_TEXT(CLAY_STRING("STATUS"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)title_font, .textColor = {245, 248, 252, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY(CLAY_ID_LOCAL("HeaderSpacer"), { .layout = { .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(1)} } }) {}
                CLAY_TEXT(g_ui.panelCollapsed ? CLAY_STRING("+") : CLAY_STRING("-"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)title_font, .textColor = {245, 248, 252, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }

            if (!g_ui.panelCollapsed) {
                CLAY(CLAY_ID_LOCAL("PanelBody"), {
                    .layout = {
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                        .padding = CLAY_PADDING_ALL((uint16_t)body_pad),
                        .childGap = (uint16_t)body_gap,
                    }
                }) {
                    CLAY_TEXT(script_ok ? CLAY_STRING("SCRIPT: OK") : CLAY_STRING("SCRIPT: ERROR"),
                              CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = script_ok ? (Clay_Color){130, 230, 130, 255} : (Clay_Color){255, 130, 120, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(mesh_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {214, 224, 237, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(selected_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = selected ? (Clay_Color){255, 230, 70, 255} : (Clay_Color){190, 200, 212, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_mode_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = sketch.active ? (Clay_Color){120, 220, 255, 255} : (Clay_Color){190, 200, 212, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_plane_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {196, 209, 226, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_profile_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = sketch.closed ? (Clay_Color){130, 230, 130, 255} : (Clay_Color){255, 210, 120, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_depth_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {196, 209, 226, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_snap_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = sketch.snapEnabled ? (Clay_Color){176, 235, 176, 255} : (Clay_Color){205, 190, 175, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_feature_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {214, 224, 237, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(face_mode_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = face_select.enabled ? (Clay_Color){138, 196, 255, 255} : (Clay_Color){190, 200, 212, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    if (face_select.enabled) {
                        CLAY_TEXT(face_region_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {196, 209, 226, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(face_hover_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {196, 209, 226, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(face_type_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {196, 209, 226, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("LCLICK PICK DETECTED FACE"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("+/- ADJUST FACE ANGLE"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    if (sketch.active) {
                        CLAY_TEXT(CLAY_STRING("LCLICK ADD  ENTER FINISH"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("C CLEAR  BKSP UNDO  X EXIT"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    if (!script_ok) {
                        CLAY_TEXT(CLAY_STRING("LAST ERROR:"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {255, 170, 170, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(err_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {255, 205, 205, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    if (!sketch.lastError.empty()) {
                        CLAY_TEXT(CLAY_STRING("SKETCH ERROR:"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {255, 170, 170, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(sketch_err_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {255, 205, 205, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                }
            }
        }
    }

    Clay_RenderCommandArray cmds = Clay_EndLayout();
    g_ui.panelData = Clay_GetElementData(CLAY_ID("StatusPanel"));
    g_ui.headerData = Clay_GetElementData(CLAY_ID("StatusHeader"));
    return cmds;
}

int main() {
    manifold::Manifold fallback = manifold::Manifold::Cube(manifold::vec3(1.0), true);
    manifold::MeshGL base_mesh = fallback.GetMeshGL();
    manifold::MeshGL mesh = base_mesh;
    Vec3 mesh_bmin = {-0.5f, -0.5f, -0.5f};
    Vec3 mesh_bmax = {0.5f, 0.5f, 0.5f};
    (void)compute_mesh_bounds(mesh, &mesh_bmin, &mesh_bmax);

    const char *script_path = "myobject.vicad";
    const char *script_out_path = "build/script_result.bin";
    const char *script_log_path = "build/script_error.log";
    const char *script_mode_env = std::getenv("VCAD_SCRIPT_MODE");
    const bool force_legacy = (script_mode_env != nullptr && std::strcmp(script_mode_env, "legacy") == 0);
    bool use_ipc_mode = !force_legacy;
    vicad::ScriptWorkerClient worker_client;
    bool ipc_start_failed = false;

    long long last_script_mtime = -1;
    std::string script_error;
    std::vector<manifold::Manifold> sketch_features;
    SketchState sketch = {
        false,            // active
        SketchPlane::XZ,  // plane
        0.0f,             // planeOffset
        1.0f,             // snapStep
        true,             // snapEnabled
        10.0f,            // extrudeDepth
        false,            // closed
        false,            // hasHover
        {0.0f, 0.0f},     // hoverUv
        {},               // points
        "",               // lastError
    };

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
    glShadeModel(GL_SMOOTH);
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_LIGHT2);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    const float global_ambient[4] = {0.14f, 0.15f, 0.17f, 1.0f};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, global_ambient);
    glClearColor(0.11f, 0.12f, 0.15f, 1.0f);
    RGFW_window_swapInterval_OpenGL(win, 1);

    const float light0_ambient[4] = {0.10f, 0.10f, 0.11f, 1.0f};
    const float light0_diffuse[4] = {0.92f, 0.95f, 1.00f, 1.0f};
    const float light0_specular[4] = {0.88f, 0.92f, 0.98f, 1.0f};
    const float light1_ambient[4] = {0.03f, 0.03f, 0.04f, 1.0f};
    const float light1_diffuse[4] = {0.44f, 0.50f, 0.58f, 1.0f};
    const float light1_specular[4] = {0.18f, 0.20f, 0.24f, 1.0f};
    const float light2_ambient[4] = {0.00f, 0.00f, 0.00f, 1.0f};
    const float light2_diffuse[4] = {0.32f, 0.33f, 0.37f, 1.0f};
    const float light2_specular[4] = {0.60f, 0.64f, 0.72f, 1.0f};
    const float mat_specular[4] = {0.58f, 0.60f, 0.64f, 1.0f};
    glLightfv(GL_LIGHT0, GL_AMBIENT, light0_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light0_diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, light0_specular);
    glLightfv(GL_LIGHT1, GL_AMBIENT, light1_ambient);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, light1_diffuse);
    glLightfv(GL_LIGHT1, GL_SPECULAR, light1_specular);
    glLightfv(GL_LIGHT2, GL_AMBIENT, light2_ambient);
    glLightfv(GL_LIGHT2, GL_DIFFUSE, light2_diffuse);
    glLightfv(GL_LIGHT2, GL_SPECULAR, light2_specular);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 52.0f);

    RGFW_event event;
    i32 width = 0;
    i32 height = 0;
    i32 window_w = 0;
    i32 window_h = 0;
    const float ui_scale = 1.0f;
    i32 ui_width = 0;
    i32 ui_height = 0;
    RGFW_window_getSizeInPixels(win, &width, &height);
    RGFW_window_getSize(win, &window_w, &window_h);
    ui_width = width;
    ui_height = height;
    clay_init(width, height);

    const float fov_degrees = 65.0f;
    Vec3 target = {0.0f, 0.0f, 0.0f};
    float yaw_deg = 45.0f;
    float pitch_deg = 24.0f;
    float distance = 80.0f;
    bool object_selected = false;
    FaceSelectState face_select = {
        false,  // enabled
        true,   // dirty
        22.0f,  // angleThresholdDeg
        -1,     // hoveredRegion
        -1,     // selectedRegion
        {},     // faces
    };
    float ui_scroll_x = 0.0f;
    float ui_scroll_y = 0.0f;
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

            bool loaded = false;
            if (use_ipc_mode) {
                loaded = worker_client.ExecuteScript(script_path, &next_mesh, &err);
                if (!loaded && !worker_client.started()) {
                    ipc_start_failed = true;
                    use_ipc_mode = false;
                    std::fprintf(stderr, "[vicad] IPC worker startup failed, falling back to legacy mode: %s\n",
                                 err.c_str());
                    err.clear();
                }
            }

            if (!loaded) {
                loaded = run_script_and_load_mesh(script_path, script_out_path, script_log_path,
                                                  &next_mesh, &next_bmin, &next_bmax, &err);
            }

            if (loaded) {
                base_mesh = std::move(next_mesh);
                std::string compose_err;
                if (!compose_scene_mesh(base_mesh, sketch_features, &mesh, &mesh_bmin, &mesh_bmax, &compose_err)) {
                    mesh = base_mesh;
                    mesh_bmin = next_bmin;
                    mesh_bmax = next_bmax;
                    sketch.lastError = compose_err;
                }
                object_selected = false;
                face_select.dirty = true;
                face_select.hoveredRegion = -1;
                face_select.selectedRegion = -1;
                script_error.clear();
                std::fprintf(stderr, "[vicad] script loaded: %s (%s)\n", script_path,
                             use_ipc_mode ? "ipc" : "legacy");
            } else {
                if (ipc_start_failed && err.empty()) {
                    err = "IPC startup failed and legacy execution also failed.";
                }
                script_error = err;
                std::fprintf(stderr, "[vicad] script error:\n%s\n", script_error.c_str());
            }
        }

        while (RGFW_window_checkEvent(win, &event)) {
            if (event.type == RGFW_quit) break;
            if (event.type == RGFW_windowResized || event.type == RGFW_scaleUpdated) {
                RGFW_window_getSizeInPixels(win, &width, &height);
                RGFW_window_getSize(win, &window_w, &window_h);
                ui_width = width;
                ui_height = height;
            }
            if (event.type == RGFW_keyPressed) {
                const RGFW_key key = event.key.value;
                if (key == RGFW_s) {
                    sketch.active = !sketch.active;
                    sketch.points.clear();
                    sketch.closed = false;
                    sketch.hasHover = false;
                    sketch.lastError.clear();
                    object_selected = false;
                    face_select.hoveredRegion = -1;
                } else if (key == RGFW_f) {
                    face_select.enabled = !face_select.enabled;
                    if (face_select.enabled) {
                        face_select.dirty = true;
                    } else {
                        face_select.hoveredRegion = -1;
                        face_select.selectedRegion = -1;
                    }
                } else if (face_select.enabled && !sketch.active &&
                           (key == RGFW_equal || key == RGFW_kpPlus)) {
                    face_select.angleThresholdDeg = clampf(face_select.angleThresholdDeg + 1.0f, 1.0f, 85.0f);
                    face_select.dirty = true;
                    face_select.selectedRegion = -1;
                } else if (face_select.enabled && !sketch.active &&
                           (key == RGFW_minus || key == RGFW_kpMinus)) {
                    face_select.angleThresholdDeg = clampf(face_select.angleThresholdDeg - 1.0f, 1.0f, 85.0f);
                    face_select.dirty = true;
                    face_select.selectedRegion = -1;
                } else if (sketch.active && key == RGFW_1) {
                    sketch.plane = SketchPlane::XY;
                    sketch.points.clear();
                    sketch.closed = false;
                    sketch.hasHover = false;
                    sketch.lastError.clear();
                } else if (sketch.active && key == RGFW_2) {
                    sketch.plane = SketchPlane::XZ;
                    sketch.points.clear();
                    sketch.closed = false;
                    sketch.hasHover = false;
                    sketch.lastError.clear();
                } else if (sketch.active && key == RGFW_3) {
                    sketch.plane = SketchPlane::YZ;
                    sketch.points.clear();
                    sketch.closed = false;
                    sketch.hasHover = false;
                    sketch.lastError.clear();
                } else if (sketch.active && key == RGFW_g) {
                    sketch.snapEnabled = !sketch.snapEnabled;
                } else if (sketch.active && key == RGFW_backSpace) {
                    if (!sketch.points.empty()) {
                        sketch.points.pop_back();
                        sketch.closed = false;
                        sketch.lastError.clear();
                    }
                } else if (sketch.active && key == RGFW_c) {
                    sketch.points.clear();
                    sketch.closed = false;
                    sketch.hasHover = false;
                    sketch.lastError.clear();
                } else if (sketch.active && key == RGFW_x) {
                    sketch.active = false;
                    sketch.points.clear();
                    sketch.closed = false;
                    sketch.hasHover = false;
                    sketch.lastError.clear();
                } else if (sketch.active && (key == RGFW_equal || key == RGFW_kpPlus)) {
                    sketch.extrudeDepth = clampf(sketch.extrudeDepth + 1.0f, 0.1f, 5000.0f);
                } else if (sketch.active && (key == RGFW_minus || key == RGFW_kpMinus)) {
                    sketch.extrudeDepth = clampf(sketch.extrudeDepth - 1.0f, 0.1f, 5000.0f);
                } else if (sketch.active && key == RGFW_bracket) {
                    const bool shift_down = RGFW_window_isKeyDown(win, RGFW_shiftL) ||
                                            RGFW_window_isKeyDown(win, RGFW_shiftR);
                    const float step = sketch.snapStep * (shift_down ? 5.0f : 1.0f);
                    sketch.planeOffset -= step;
                } else if (sketch.active && key == RGFW_closeBracket) {
                    const bool shift_down = RGFW_window_isKeyDown(win, RGFW_shiftL) ||
                                            RGFW_window_isKeyDown(win, RGFW_shiftR);
                    const float step = sketch.snapStep * (shift_down ? 5.0f : 1.0f);
                    sketch.planeOffset += step;
                } else if (sketch.active && (key == RGFW_enter || key == RGFW_kpReturn)) {
                    if (!sketch.closed && sketch.points.size() >= 3) {
                        sketch.closed = true;
                    }
                    manifold::Manifold feature;
                    std::string err;
                    if (build_sketch_feature(sketch, &feature, &err)) {
                        sketch_features.push_back(std::move(feature));
                        std::string compose_err;
                        if (compose_scene_mesh(base_mesh, sketch_features, &mesh, &mesh_bmin, &mesh_bmax, &compose_err)) {
                            sketch.points.clear();
                            sketch.closed = false;
                            sketch.hasHover = false;
                            sketch.lastError.clear();
                            object_selected = false;
                            face_select.dirty = true;
                            face_select.hoveredRegion = -1;
                            face_select.selectedRegion = -1;
                        } else {
                            sketch_features.pop_back();
                            sketch.lastError = compose_err;
                        }
                    } else {
                        sketch.lastError = err;
                    }
                }
            }
            if (event.type == RGFW_mouseScroll) {
                distance *= std::pow(0.9f, event.scroll.y);
                distance = clampf(distance, 0.25f, 2000.0f);
                ui_scroll_x += event.scroll.x;
                ui_scroll_y += event.scroll.y;
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
                    i32 mouse_ui_x = mouse_px_x;
                    i32 mouse_ui_y = mouse_px_y;

                    if (g_ui.headerData.found &&
                        point_in_rect(mouse_ui_x, mouse_ui_y,
                                      (int)g_ui.headerData.boundingBox.x, (int)g_ui.headerData.boundingBox.y,
                                      (int)g_ui.headerData.boundingBox.width, (int)g_ui.headerData.boundingBox.height)) {
                        g_ui.panelCollapsed = !g_ui.panelCollapsed;
                        continue;
                    }
                    if (g_ui.panelData.found &&
                        point_in_rect(mouse_ui_x, mouse_ui_y,
                                      (int)g_ui.panelData.boundingBox.x, (int)g_ui.panelData.boundingBox.y,
                                      (int)g_ui.panelData.boundingBox.width, (int)g_ui.panelData.boundingBox.height)) {
                        // Click on panel body should not affect 3D selection.
                        continue;
                    }

                    if (sketch.active) {
                        const Vec3 eye = camera_position(target, yaw_deg, pitch_deg, distance);
                        const CameraBasis basis = camera_basis(eye, target);
                        Vec2 uv = {0.0f, 0.0f};
                        if (mouse_to_sketch_uv(mouse_px_x, mouse_px_y, width, height,
                                               eye, basis, fov_degrees, sketch, &uv, nullptr)) {
                            if (!sketch.closed && sketch.points.size() >= 3) {
                                const float close_t = sketch_close_threshold(sketch);
                                if (length2_2d(sub2(uv, sketch.points.front())) <= close_t * close_t) {
                                    sketch.closed = true;
                                    sketch.hoverUv = sketch.points.front();
                                    sketch.hasHover = true;
                                    sketch.lastError.clear();
                                    continue;
                                }
                            }
                            if (sketch.closed) {
                                sketch.points.clear();
                                sketch.closed = false;
                            }
                            if (sketch.points.empty() ||
                                length2_2d(sub2(uv, sketch.points.back())) > 1e-8f) {
                                sketch.points.push_back(uv);
                                sketch.lastError.clear();
                            }
                        } else {
                            sketch.lastError = "Sketch ray did not hit active plane.";
                        }
                        continue;
                    }

                    const Vec3 eye = camera_position(target, yaw_deg, pitch_deg, distance);
                    const CameraBasis basis = camera_basis(eye, target);
                    const Vec3 ray_dir =
                        camera_ray_direction(mouse_px_x, mouse_px_y, width, height, fov_degrees, basis);
                    if (face_select.enabled) {
                        if (face_select.dirty) {
                            face_select.faces = vicad::DetectMeshFaces(mesh, face_select.angleThresholdDeg);
                            face_select.dirty = false;
                        }
                        const int region = vicad::PickFaceRegionByRay(
                            mesh, face_select.faces,
                            (double)eye.x, (double)eye.y, (double)eye.z,
                            (double)ray_dir.x, (double)ray_dir.y, (double)ray_dir.z,
                            nullptr);
                        face_select.selectedRegion = region;
                        object_selected = region >= 0;
                    } else {
                        object_selected = ray_hits_aabb(eye, ray_dir, mesh_bmin, mesh_bmax);
                    }
                }
            }
        }

        if (height <= 0) height = 1;
        if (ui_height <= 0) ui_height = 1;
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

        const Vec3 key = light_anchor(eye, basis, distance * 0.72f, distance * 0.95f, distance * 0.38f);
        const Vec3 fill = light_anchor(eye, basis, -distance * 0.90f, distance * 0.25f, distance * 0.10f);
        const Vec3 rim = light_anchor(eye, basis, distance * 0.15f, distance * 0.56f, -distance * 1.25f);
        const float key_pos[4] = {key.x, key.y, key.z, 1.0f};
        const float fill_pos[4] = {fill.x, fill.y, fill.z, 1.0f};
        const float rim_pos[4] = {rim.x, rim.y, rim.z, 1.0f};
        glLightfv(GL_LIGHT0, GL_POSITION, key_pos);
        glLightfv(GL_LIGHT1, GL_POSITION, fill_pos);
        glLightfv(GL_LIGHT2, GL_POSITION, rim_pos);

        i32 mouse_x = 0;
        i32 mouse_y = 0;
        RGFW_window_getMouse(win, &mouse_x, &mouse_y);
        i32 mouse_px_x = 0;
        i32 mouse_px_y = 0;
        window_mouse_to_pixel(mouse_x, mouse_y, window_w, window_h, width, height,
                              &mouse_px_x, &mouse_px_y);
        i32 mouse_ui_x = mouse_px_x;
        i32 mouse_ui_y = mouse_px_y;
        Clay_SetCurrentContext(g_ui.ctx);
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)ui_width, (float)ui_height});
        Clay_SetPointerState((Clay_Vector2){(float)mouse_ui_x, (float)mouse_ui_y},
                             RGFW_window_isMouseDown(win, RGFW_mouseLeft));
        Clay_UpdateScrollContainers(true, (Clay_Vector2){ui_scroll_x, ui_scroll_y}, 0.016f);
        ui_scroll_x = 0.0f;
        ui_scroll_y = 0.0f;

        if (sketch.active) {
            Vec2 uv = {0.0f, 0.0f};
            if (mouse_to_sketch_uv(mouse_px_x, mouse_px_y, width, height,
                                   eye, basis, fov_degrees, sketch, &uv, nullptr)) {
                if (!sketch.closed && sketch.points.size() >= 3) {
                    const float close_t = sketch_close_threshold(sketch);
                    if (length2_2d(sub2(uv, sketch.points.front())) <= close_t * close_t) {
                        uv = sketch.points.front();
                    }
                }
                sketch.hoverUv = uv;
                sketch.hasHover = true;
            } else {
                sketch.hasHover = false;
            }
        } else {
            sketch.hasHover = false;
        }

        if (face_select.enabled && !sketch.active) {
            if (face_select.dirty) {
                face_select.faces = vicad::DetectMeshFaces(mesh, face_select.angleThresholdDeg);
                face_select.dirty = false;
                if ((size_t)face_select.selectedRegion >= face_select.faces.regions.size()) {
                    face_select.selectedRegion = -1;
                }
            }
            const Vec3 ray_dir =
                camera_ray_direction(mouse_px_x, mouse_px_y, width, height, fov_degrees, basis);
            face_select.hoveredRegion = vicad::PickFaceRegionByRay(
                mesh, face_select.faces,
                (double)eye.x, (double)eye.y, (double)eye.z,
                (double)ray_dir.x, (double)ray_dir.y, (double)ray_dir.z,
                nullptr);
        } else {
            face_select.hoveredRegion = -1;
        }

        Clay_RenderCommandArray ui_cmds = build_clay_ui(
            ui_width, ui_height, script_error.empty(), script_error,
            mesh.NumTri(), mesh.NumVert(), object_selected, sketch, sketch_features.size(), face_select);

        draw_grid();
        draw_sketch_plane_grid(sketch);
        draw_mesh(mesh);
        draw_mesh_edge_strokes(mesh);
        if (face_select.enabled) {
            if (face_select.hoveredRegion >= 0 &&
                face_select.hoveredRegion != face_select.selectedRegion) {
                draw_face_region_overlay(mesh, face_select.faces, face_select.hoveredRegion,
                                         0.34f, 0.66f, 1.00f, 0.18f);
            }
            if (face_select.selectedRegion >= 0) {
                draw_face_region_overlay(mesh, face_select.faces, face_select.selectedRegion,
                                         0.22f, 0.52f, 0.98f, 0.32f);
            }
        }
        if (object_selected && !face_select.enabled) draw_selected_outline(mesh);
        draw_sketch_profile(sketch);
        const int top_right_offset =
            (g_ui.panelData.found ? (int)std::lround(g_ui.panelData.boundingBox.height) : 0) + 8;
        draw_orientation_cube(basis, width, height, top_right_offset);
        clay_render_commands(ui_cmds, width, height, ui_scale);

        RGFW_window_swapBuffers_OpenGL(win);
    }

    RGFW_window_close(win);
    return 0;
}
