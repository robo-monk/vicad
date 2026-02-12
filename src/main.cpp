#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <cstdint>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#define RGFW_OPENGL
#define RGFW_IMPLEMENTATION
#include "../RGFW.h"
#include "edge_detection.h"
#include "face_detection.h"
#include "app_state.h"
#include "input_controller.h"
#include "render_scene.h"
#include "render_ui.h"
#include "scene_runtime.h"
#include "script_worker_client.h"
#include "manifold/manifold.h"
#include "manifold/cross_section.h"
#include "manifold/meshIO.h"
#include "lod_policy.h"

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#define CLAY_IMPLEMENTATION
#include "../clay.h"
#include "funnel_sans_baked.h"

using vicad_app::Vec2;
using vicad_app::Vec3;
using vicad_app::FaceSelectState;
using vicad_app::EdgeSelectState;
using vicad_app::CameraBasis;
using vicad_app::DimensionRenderContext;

struct ClayUiState {
    Clay_Arena arena;
    Clay_Context *ctx;
    bool initialized;
    bool panelCollapsed;
    Clay_ElementData panelData;
    Clay_ElementData headerData;
    Clay_ElementData infoPanelData;
    Clay_ElementData exportPanelData;
    Clay_ElementData exportButtonData;
    std::array<std::string, 128> textSlots;
};

static ClayUiState g_ui = {};

static Clay_String ui_text_slot(size_t slot, const char *text) {
    if (slot >= g_ui.textSlots.size()) {
        return CLAY_STRING("");
    }
    g_ui.textSlots[slot] = text ? text : "";
    Clay_String s = {
        .length = (int32_t)g_ui.textSlots[slot].size(),
        .chars = g_ui.textSlots[slot].c_str(),
        .isStaticallyAllocated = false,
    };
    return s;
}

static Clay_String ui_text_slot(size_t slot, const std::string &text) {
    if (slot >= g_ui.textSlots.size()) {
        return CLAY_STRING("");
    }
    g_ui.textSlots[slot] = text;
    Clay_String s = {
        .length = (int32_t)g_ui.textSlots[slot].size(),
        .chars = g_ui.textSlots[slot].c_str(),
        .isStaticallyAllocated = false,
    };
    return s;
}

struct BakedFontState {
    GLuint texture;
    bool initialized;
};

static BakedFontState g_baked_font = {};
static constexpr float kHudBaseScale = 0.75f;
static constexpr float kHudLegacyScale = 1.5f;
static constexpr int kRequestedMsaaSamples = 4;
static constexpr float kTextShadowAlphaScale = 0.18f;

static const VicadBakedGlyph *glyph_for_char(unsigned char ch) {
    if (ch < VICAD_BAKED_FIRST_CHAR || ch > VICAD_BAKED_LAST_CHAR) ch = '?';
    return &vicad_baked_glyphs[ch - VICAD_BAKED_FIRST_CHAR];
}

static float text_width_for_slice(Clay_StringSlice text, float font_px) {
    if (text.length <= 0 || !text.chars || font_px <= 0.0f) return 0.0f;
    const float scale = font_px / (float)VICAD_BAKED_PIXEL_SIZE;
    float width = 0.0f;
    float line_width = 0.0f;
    for (int i = 0; i < text.length; ++i) {
        const unsigned char ch = (unsigned char)text.chars[i];
        if (ch == '\n') {
            if (line_width > width) width = line_width;
            line_width = 0.0f;
            continue;
        }
        line_width += (float)glyph_for_char(ch)->advance * scale;
    }
    if (line_width > width) width = line_width;
    return width;
}

static float text_width_for_cstr(const char *text, float font_px) {
    if (!text) return 0.0f;
    Clay_StringSlice s = {
        .length = (int32_t)std::strlen(text),
        .chars = text,
    };
    return text_width_for_slice(s, font_px);
}

static Clay_Dimensions measure_text_mono(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    const float font_px = (float)(config ? config->fontSize : 16);
    const float scale = font_px / (float)VICAD_BAKED_PIXEL_SIZE;
    const float width = text_width_for_slice(text, font_px);
    int lines = 1;
    for (int i = 0; i < text.length; ++i) {
        if (text.chars && text.chars[i] == '\n') lines += 1;
    }
    const float height = (float)VICAD_BAKED_LINE_HEIGHT * scale * (float)lines;
    return (Clay_Dimensions){width, height};
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

static bool ensure_baked_font_texture(void) {
    if (g_baked_font.initialized) return true;
    glGenTextures(1, &g_baked_font.texture);
    if (g_baked_font.texture == 0) return false;

    glBindTexture(GL_TEXTURE_2D, g_baked_font.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_ALPHA,
                 VICAD_BAKED_ATLAS_WIDTH,
                 VICAD_BAKED_ATLAS_HEIGHT,
                 0,
                 GL_ALPHA,
                 GL_UNSIGNED_BYTE,
                 vicad_baked_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_baked_font.initialized = true;
    return true;
}

static void draw_text_5x7(float x, float y, float scale, const char *text, float r, float g, float b);
static void draw_text_from_slice(float x, float y, float scale, Clay_StringSlice s, float r, float g, float b);
static void draw_text_baked_world(const Vec3 &origin,
                                  const Vec3 &axis_right,
                                  const Vec3 &axis_up,
                                  float world_scale, const char *text,
                                  float r, float g, float b, float a);

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
                    x0, y0,
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

using vicad_app::add;
using vicad_app::mul;
using vicad_app::sub;
using vicad_app::cross;
using vicad_app::normalize;
using vicad_app::dot;
using vicad_app::clampf;

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

static Vec3 mesh_vertex(const manifold::MeshGL &mesh, uint32_t idx) {
    return {
        mesh.vertProperties[(size_t)idx * mesh.numProp + 0],
        mesh.vertProperties[(size_t)idx * mesh.numProp + 1],
        mesh.vertProperties[(size_t)idx * mesh.numProp + 2],
    };
}

static void draw_edge_indices(const manifold::MeshGL &mesh,
                              const vicad::EdgeDetectionResult &edge_result,
                              const std::vector<int> &indices) {
    glBegin(GL_LINES);
    for (const int idx : indices) {
        if (idx < 0 || (size_t)idx >= edge_result.edges.size()) continue;
        const vicad::EdgeRecord &e = edge_result.edges[(size_t)idx];
        const Vec3 p0 = mesh_vertex(mesh, e.v0);
        const Vec3 p1 = mesh_vertex(mesh, e.v1);
        glVertex3f(p0.x, p0.y, p0.z);
        glVertex3f(p1.x, p1.y, p1.z);
    }
    glEnd();
}

static void draw_feature_edges(const manifold::MeshGL &mesh,
                               const vicad::EdgeDetectionResult &edge_result) {
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glLineWidth(2.0f);
    glColor4f(0.06f, 0.11f, 0.16f, 0.90f);
    draw_edge_indices(mesh, edge_result, edge_result.sharpEdgeIndices);

    glLineWidth(2.4f);
    glColor4f(0.05f, 0.13f, 0.20f, 0.96f);
    draw_edge_indices(mesh, edge_result, edge_result.boundaryEdgeIndices);

    glLineWidth(2.8f);
    glColor4f(0.98f, 0.38f, 0.30f, 0.98f);
    draw_edge_indices(mesh, edge_result, edge_result.nonManifoldEdgeIndices);

    glLineWidth(1.0f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void draw_silhouette_edges(const manifold::MeshGL &mesh,
                                  const vicad::EdgeDetectionResult &edge_result,
                                  const vicad::SilhouetteResult &silhouette) {
    if (silhouette.silhouetteEdgeIndices.empty()) return;
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(2.6f);
    glColor4f(0.04f, 0.08f, 0.12f, 0.98f);
    draw_edge_indices(mesh, edge_result, silhouette.silhouetteEdgeIndices);
    glLineWidth(1.0f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void draw_selected_edge(const manifold::MeshGL &mesh,
                               const vicad::EdgeDetectionResult &edge_result,
                               int edge_index) {
    if (edge_index < 0 || (size_t)edge_index >= edge_result.edges.size()) return;
    const vicad::EdgeRecord &e = edge_result.edges[(size_t)edge_index];
    const Vec3 p0 = mesh_vertex(mesh, e.v0);
    const Vec3 p1 = mesh_vertex(mesh, e.v1);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(4.8f);
    glColor4f(1.0f, 0.90f, 0.16f, 1.0f);
    glBegin(GL_LINES);
    glVertex3f(p0.x, p0.y, p0.z);
    glVertex3f(p1.x, p1.y, p1.z);
    glEnd();
    glLineWidth(1.0f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void draw_hovered_edge(const manifold::MeshGL &mesh,
                              const vicad::EdgeDetectionResult &edge_result,
                              int edge_index) {
    if (edge_index < 0 || (size_t)edge_index >= edge_result.edges.size()) return;
    const vicad::EdgeRecord &e = edge_result.edges[(size_t)edge_index];
    const Vec3 p0 = mesh_vertex(mesh, e.v0);
    const Vec3 p1 = mesh_vertex(mesh, e.v1);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(3.6f);
    glColor4f(0.38f, 0.73f, 1.0f, 0.96f);
    glBegin(GL_LINES);
    glVertex3f(p0.x, p0.y, p0.z);
    glVertex3f(p1.x, p1.y, p1.z);
    glEnd();
    glLineWidth(1.0f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void draw_chain_overlay(const manifold::MeshGL &mesh,
                               const vicad::EdgeDetectionResult &edge_result,
                               int chain_index,
                               float line_width,
                               float r, float g, float b, float a) {
    if (chain_index < 0 || (size_t)chain_index >= edge_result.featureChains.size()) return;
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(line_width);
    glColor4f(r, g, b, a);
    draw_edge_indices(mesh, edge_result, edge_result.featureChains[(size_t)chain_index]);
    glLineWidth(1.0f);
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

static void draw_mesh_selection_overlay(const manifold::MeshGL &mesh,
                                        float r, float g, float b, float a) {
    if (mesh.NumTri() == 0) return;

    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glColor4f(r, g, b, a);

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

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static bool scene_object_is_manifold(const vicad::ScriptSceneObject &obj) {
    return obj.kind == vicad::ScriptSceneObjectKind::Manifold;
}

static bool scene_object_is_sketch(const vicad::ScriptSceneObject &obj) {
    return obj.kind == vicad::ScriptSceneObjectKind::CrossSection;
}

static size_t count_scene_sketches(const std::vector<vicad::ScriptSceneObject> &scene) {
    size_t count = 0;
    for (const vicad::ScriptSceneObject &obj : scene) {
        if (scene_object_is_sketch(obj)) {
            count++;
        }
    }
    return count;
}

static bool compute_scene_bounds(const std::vector<vicad::ScriptSceneObject> &scene,
                                 Vec3 *out_min, Vec3 *out_max) {
    bool have = false;
    Vec3 mn = {0.0f, 0.0f, 0.0f};
    Vec3 mx = {0.0f, 0.0f, 0.0f};
    for (const vicad::ScriptSceneObject &obj : scene) {
        const Vec3 obmin = {obj.bmin.x, obj.bmin.y, obj.bmin.z};
        const Vec3 obmax = {obj.bmax.x, obj.bmax.y, obj.bmax.z};
        if (!have) {
            mn = obmin;
            mx = obmax;
            have = true;
            continue;
        }
        if (obmin.x < mn.x) mn.x = obmin.x;
        if (obmin.y < mn.y) mn.y = obmin.y;
        if (obmin.z < mn.z) mn.z = obmin.z;
        if (obmax.x > mx.x) mx.x = obmax.x;
        if (obmax.y > mx.y) mx.y = obmax.y;
        if (obmax.z > mx.z) mx.z = obmax.z;
    }
    if (!have) return false;
    *out_min = mn;
    *out_max = mx;
    return true;
}

static bool selected_scene_object_bounds(const std::vector<vicad::ScriptSceneObject> &scene,
                                         int selected_object_index,
                                         Vec3 *out_min, Vec3 *out_max) {
    if (selected_object_index < 0) return false;
    const size_t idx = (size_t)selected_object_index;
    if (idx >= scene.size()) return false;
    const vicad::ScriptSceneObject &obj = scene[idx];
    *out_min = {obj.bmin.x, obj.bmin.y, obj.bmin.z};
    *out_max = {obj.bmax.x, obj.bmax.y, obj.bmax.z};
    return true;
}

static std::string format_op_trace_entry(const vicad::OpTraceEntry &entry) {
    std::ostringstream ss;
    ss << entry.name << "(";
    for (size_t i = 0; i < entry.args.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << entry.args[i];
    }
    ss << ")";
    return ss.str();
}

static void focus_camera_on_bounds(const Vec3 &bmin, const Vec3 &bmax,
                                   float fov_degrees,
                                   float *inout_distance,
                                   Vec3 *inout_target) {
    if (!inout_distance || !inout_target) return;
    const Vec3 center = mul(add(bmin, bmax), 0.5f);
    const Vec3 diag = sub(bmax, bmin);
    float radius = 0.5f * std::sqrt(dot(diag, diag));
    if (radius < 1e-3f) radius = 1e-3f;
    const float half_fov = (fov_degrees * 3.1415926535f / 180.0f) * 0.5f;
    float fit_distance = radius / std::tan(half_fov);
    fit_distance *= 1.35f;
    fit_distance = clampf(fit_distance, 0.25f, 2000.0f);
    *inout_target = center;
    *inout_distance = fit_distance;
}

static void draw_script_sketch_object(const vicad::ScriptSceneObject &obj,
                                      float line_width,
                                      float r, float g, float b, float a) {
    if (!scene_object_is_sketch(obj)) return;
    if (obj.sketchContours.empty()) return;

    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glLineWidth(line_width);
    glColor4f(r, g, b, a);

    for (const vicad::ScriptSketchContour &contour : obj.sketchContours) {
        if (contour.points.size() < 2) continue;
        glBegin(GL_LINE_LOOP);
        for (const vicad::SceneVec3 &p : contour.points) {
            glVertex3f(p.x, p.y, p.z);
        }
        glEnd();
    }

    glLineWidth(1.0f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
}

static void draw_script_sketches(const std::vector<vicad::ScriptSceneObject> &scene,
                                 int selected_object_index,
                                 int hovered_object_index) {
    for (size_t i = 0; i < scene.size(); ++i) {
        if (!scene_object_is_sketch(scene[i])) continue;
        const bool selected = ((int)i == selected_object_index);
        const bool hovered = ((int)i == hovered_object_index);
        if (selected) {
            draw_script_sketch_object(scene[i], 4.0f, 1.0f, 0.86f, 0.18f, 1.0f);
        } else if (hovered) {
            draw_script_sketch_object(scene[i], 3.2f, 0.34f, 0.76f, 1.0f, 0.96f);
        } else {
            draw_script_sketch_object(scene[i], 2.2f, 0.24f, 0.70f, 0.96f, 0.92f);
        }
    }
}

enum class SketchDimShapeKind {
    Unknown,
    Point,
    Circle,
    Rectangle,
    Square,
    RegularPolygon,
    Polygon,
};

using vicad_app::add2;
using vicad_app::sub2v;
using vicad_app::mul2;
using vicad_app::dot2;
using vicad_app::length2;
using vicad_app::normalize2;
using vicad_app::perp2;
using vicad_app::vec3_from_2d;

static void contour_plane_axes(const vicad::ScriptSketchContour &contour,
                               Vec3 *out_right,
                               Vec3 *out_up,
                               Vec3 *out_normal) {
    Vec3 right = {1.0f, 0.0f, 0.0f};
    Vec3 up = {0.0f, 1.0f, 0.0f};
    Vec3 normal = {0.0f, 0.0f, 1.0f};
    if (contour.points.size() >= 2) {
        const auto to_v3 = [](const vicad::SceneVec3 &p) {
            return Vec3{p.x, p.y, p.z};
        };
        const Vec3 p0 = to_v3(contour.points[0]);
        bool found_right = false;
        for (size_t i = 1; i < contour.points.size(); ++i) {
            const Vec3 e = sub(to_v3(contour.points[i]), p0);
            if (dot(e, e) > 1e-10f) {
                right = normalize(e);
                found_right = true;
                break;
            }
        }
        if (found_right) {
            bool found_normal = false;
            for (size_t i = 1; i < contour.points.size(); ++i) {
                const Vec3 e = sub(to_v3(contour.points[i]), p0);
                const Vec3 n = cross(right, e);
                if (dot(n, n) > 1e-10f) {
                    normal = normalize(n);
                    found_normal = true;
                    break;
                }
            }
            if (!found_normal) {
                const Vec3 fallback_up = std::fabs(right.z) < 0.95f
                    ? Vec3{0.0f, 0.0f, 1.0f}
                    : Vec3{0.0f, 1.0f, 0.0f};
                normal = normalize(cross(right, fallback_up));
            }
            up = normalize(cross(normal, right));
        }
    }
    if (out_right) *out_right = right;
    if (out_up) *out_up = up;
    if (out_normal) *out_normal = normal;
}

static float contour_signed_area(const std::vector<Vec2> &pts) {
    const size_t n = pts.size();
    if (n < 3) return 0.0f;
    double a = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Vec2 &p0 = pts[i];
        const Vec2 &p1 = pts[(i + 1) % n];
        a += (double)p0.x * (double)p1.y - (double)p1.x * (double)p0.y;
    }
    return (float)(0.5 * a);
}

static void contour_bbox(const std::vector<Vec2> &pts, Vec2 *mn, Vec2 *mx) {
    Vec2 bmin = {0.0f, 0.0f};
    Vec2 bmax = {0.0f, 0.0f};
    if (!pts.empty()) {
        bmin = pts[0];
        bmax = pts[0];
        for (size_t i = 1; i < pts.size(); ++i) {
            const Vec2 &p = pts[i];
            if (p.x < bmin.x) bmin.x = p.x;
            if (p.y < bmin.y) bmin.y = p.y;
            if (p.x > bmax.x) bmax.x = p.x;
            if (p.y > bmax.y) bmax.y = p.y;
        }
    }
    *mn = bmin;
    *mx = bmax;
}

static Vec2 contour_centroid_mean(const std::vector<Vec2> &pts) {
    if (pts.empty()) return {0.0f, 0.0f};
    Vec2 c = {0.0f, 0.0f};
    for (const Vec2 &p : pts) c = add2(c, p);
    const float inv = 1.0f / (float)pts.size();
    return mul2(c, inv);
}

static bool classify_rectangle_like(const std::vector<Vec2> &pts,
                                    float *out_w, float *out_h) {
    if (pts.size() != 4) return false;
    const Vec2 e0 = sub2v(pts[1], pts[0]);
    const Vec2 e1 = sub2v(pts[2], pts[1]);
    const Vec2 e2 = sub2v(pts[3], pts[2]);
    const Vec2 e3 = sub2v(pts[0], pts[3]);
    const float l0 = length2(e0);
    const float l1 = length2(e1);
    const float l2 = length2(e2);
    const float l3 = length2(e3);
    if (l0 <= 1e-6f || l1 <= 1e-6f || l2 <= 1e-6f || l3 <= 1e-6f) return false;
    const float orth = std::fabs(dot2(normalize2(e0), normalize2(e1)));
    const float opp0 = std::fabs(l0 - l2) / ((l0 > l2) ? l0 : l2);
    const float opp1 = std::fabs(l1 - l3) / ((l1 > l3) ? l1 : l3);
    if (orth > 0.05f || opp0 > 0.05f || opp1 > 0.05f) return false;
    *out_w = l0;
    *out_h = l1;
    return true;
}

static bool classify_circle_like(const std::vector<Vec2> &pts,
                                 Vec2 *out_center, float *out_radius) {
    const size_t n = pts.size();
    if (n < 12) return false;
    const Vec2 c = contour_centroid_mean(pts);
    double sum_r = 0.0;
    std::vector<float> rs;
    rs.reserve(n);
    for (const Vec2 &p : pts) {
        const float r = length2(sub2v(p, c));
        rs.push_back(r);
        sum_r += (double)r;
    }
    const float mean_r = (float)(sum_r / (double)n);
    if (mean_r <= 1e-6f) return false;
    double max_dev = 0.0;
    for (float r : rs) {
        const double d = std::fabs((double)r - (double)mean_r);
        if (d > max_dev) max_dev = d;
    }
    const double rel = max_dev / (double)mean_r;
    if (rel > 0.03) return false;
    *out_center = c;
    *out_radius = mean_r;
    return true;
}

static bool classify_rounded_rectangle_like(const std::vector<Vec2> &pts,
                                            float *out_w, float *out_h) {
    const size_t n = pts.size();
    if (n < 8) return false;

    Vec2 bmin = {0.0f, 0.0f};
    Vec2 bmax = {0.0f, 0.0f};
    contour_bbox(pts, &bmin, &bmax);
    const float bw = bmax.x - bmin.x;
    const float bh = bmax.y - bmin.y;
    if (bw <= 1e-5f || bh <= 1e-5f) return false;

    const float max_dim = (bw > bh) ? bw : bh;
    const float side_tol = max_dim * 0.02f + 1e-4f;

    bool touch_left = false;
    bool touch_right = false;
    bool touch_bottom = false;
    bool touch_top = false;
    for (const Vec2 &p : pts) {
        if (std::fabs(p.x - bmin.x) <= side_tol) touch_left = true;
        if (std::fabs(p.x - bmax.x) <= side_tol) touch_right = true;
        if (std::fabs(p.y - bmin.y) <= side_tol) touch_bottom = true;
        if (std::fabs(p.y - bmax.y) <= side_tol) touch_top = true;
    }
    if (!touch_left || !touch_right || !touch_bottom || !touch_top) return false;

    // Rounded rectangles keep a meaningful amount of axis-aligned edge length.
    double total_len = 0.0;
    double axis_len = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Vec2 &a = pts[i];
        const Vec2 &b = pts[(i + 1) % n];
        const Vec2 e = sub2v(b, a);
        const float len = length2(e);
        if (len <= 1e-6f) continue;
        total_len += (double)len;
        const Vec2 dir = {e.x / len, e.y / len};
        const float axis_score = std::fmax(std::fabs(dir.x), std::fabs(dir.y));
        if (axis_score >= 0.9659f) {  // ~15 degrees from axis
            axis_len += (double)len;
        }
    }
    if (total_len <= 1e-8) return false;
    const double axis_ratio = axis_len / total_len;
    if (axis_ratio < 0.18) return false;

    *out_w = bw;
    *out_h = bh;
    return true;
}

static bool classify_regular_polygon_like(const std::vector<Vec2> &pts,
                                          Vec2 *out_center,
                                          float *out_radius,
                                          float *out_side) {
    const size_t n = pts.size();
    if (n < 3 || n > 16) return false;
    const Vec2 c = contour_centroid_mean(pts);
    double sum_r = 0.0;
    std::vector<float> rs;
    rs.reserve(n);
    for (const Vec2 &p : pts) {
        const float r = length2(sub2v(p, c));
        rs.push_back(r);
        sum_r += (double)r;
    }
    const float mean_r = (float)(sum_r / (double)n);
    if (mean_r <= 1e-6f) return false;
    double max_rel_r = 0.0;
    for (float r : rs) {
        const double rel = std::fabs((double)r - (double)mean_r) / (double)mean_r;
        if (rel > max_rel_r) max_rel_r = rel;
    }
    if (max_rel_r > 0.03) return false;

    std::vector<float> edge_lengths;
    edge_lengths.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec2 &a = pts[i];
        const Vec2 &b = pts[(i + 1) % n];
        edge_lengths.push_back(length2(sub2v(b, a)));
    }
    float mean_e = 0.0f;
    for (float e : edge_lengths) mean_e += e;
    mean_e /= (float)n;
    if (mean_e <= 1e-6f) return false;
    double max_rel_e = 0.0;
    for (float e : edge_lengths) {
        const double rel = std::fabs((double)e - (double)mean_e) / (double)mean_e;
        if (rel > max_rel_e) max_rel_e = rel;
    }
    if (max_rel_e > 0.04) return false;
    *out_center = c;
    *out_radius = mean_r;
    *out_side = mean_e;
    return true;
}

static void draw_world_line(const Vec3 &a, const Vec3 &b) {
    glBegin(GL_LINES);
    glVertex3f(a.x, a.y, a.z);
    glVertex3f(b.x, b.y, b.z);
    glEnd();
}

static void format_dim(char *buf, size_t cap, const char *prefix, float v) {
    if (!buf || cap == 0) return;
    std::snprintf(buf, cap, "%s%.3g", prefix ? prefix : "", (double)v);
}

static Vec3 project_onto_plane(const Vec3 &v, const Vec3 &plane_normal) {
    return sub(v, mul(plane_normal, dot(v, plane_normal)));
}

static void orient_label_axes_for_camera(const Vec3 &eye,
                                         const CameraBasis &camera,
                                         const Vec3 &anchor,
                                         const Vec3 &plane_normal,
                                         const Vec3 &fallback_right,
                                         Vec3 *out_right,
                                         Vec3 *out_up,
                                         Vec3 *out_facing_normal) {
    Vec3 facing_normal = normalize(plane_normal);
    const Vec3 to_eye = normalize(sub(eye, anchor));
    if (dot(facing_normal, to_eye) < 0.0f) {
        facing_normal = mul(facing_normal, -1.0f);
    }

    Vec3 right = project_onto_plane(camera.right, facing_normal);
    if (dot(right, right) <= 1e-8f) {
        right = project_onto_plane(fallback_right, facing_normal);
    }
    if (dot(right, right) <= 1e-8f) {
        right = project_onto_plane(camera.up, facing_normal);
    }
    if (dot(right, right) <= 1e-8f) {
        right = {1.0f, 0.0f, 0.0f};
    }
    right = normalize(right);
    Vec3 up = normalize(cross(facing_normal, right));
    if (dot(up, camera.up) < 0.0f) {
        right = mul(right, -1.0f);
        up = mul(up, -1.0f);
    }
    if (out_right) *out_right = right;
    if (out_up) *out_up = up;
    if (out_facing_normal) *out_facing_normal = facing_normal;
}

static float text_width_world(const char *text, float world_scale) {
    const float font_px = (float)VICAD_BAKED_PIXEL_SIZE * world_scale;
    return text_width_for_cstr(text, font_px);
}

static void draw_dimension_label_world(const Vec3 &anchor,
                                       const Vec3 &axis_right,
                                       const Vec3 &axis_up,
                                       float world_scale,
                                       const char *text,
                                       float r, float g, float b, float a,
                                       bool center_x) {
    if (!text || !text[0]) return;
    const float label_world_scale = world_scale;
    Vec3 origin = anchor;
    if (center_x) {
        origin = add(origin, mul(axis_right, -0.5f * text_width_world(text, label_world_scale)));
    }
    draw_text_baked_world(origin, axis_right, axis_up, label_world_scale, text, r, g, b, a);
}

static void draw_dimension_arrowheads_world(const Vec3 &a,
                                            const Vec3 &b,
                                            const Vec3 &plane_normal,
                                            float size) {
    const Vec3 dim = sub(b, a);
    const float len2 = dot(dim, dim);
    if (len2 <= 1e-10f || size <= 0.0f) return;
    const float len = std::sqrt(len2);
    Vec3 dir = mul(dim, 1.0f / len);
    Vec3 side = normalize(cross(plane_normal, dir));
    if (dot(side, side) <= 1e-8f) {
        side = normalize(cross({0.0f, 1.0f, 0.0f}, dir));
    }
    if (dot(side, side) <= 1e-8f) {
        side = {1.0f, 0.0f, 0.0f};
    }
    const float arrow_len = clampf(size, len * 0.08f, len * 0.30f);
    const float arrow_half_w = arrow_len * 0.52f;
    const Vec3 tail_a = add(a, mul(dir, arrow_len));
    const Vec3 tail_b = add(b, mul(dir, -arrow_len));
    const Vec3 a_left = add(tail_a, mul(side, arrow_half_w));
    const Vec3 a_right = add(tail_a, mul(side, -arrow_half_w));
    const Vec3 b_left = add(tail_b, mul(side, arrow_half_w));
    const Vec3 b_right = add(tail_b, mul(side, -arrow_half_w));
    glBegin(GL_TRIANGLES);
    glVertex3f(a.x, a.y, a.z);
    glVertex3f(a_left.x, a_left.y, a_left.z);
    glVertex3f(a_right.x, a_right.y, a_right.z);
    glVertex3f(b.x, b.y, b.z);
    glVertex3f(b_right.x, b_right.y, b_right.z);
    glVertex3f(b_left.x, b_left.y, b_left.z);
    glEnd();
}

static float world_per_pixel_at_anchor(const DimensionRenderContext &ctx, const Vec3 &anchor) {
    const float vh = (float)(ctx.viewportHeight > 0 ? ctx.viewportHeight : 1);
    const float half_fov = (ctx.fovDegrees * 3.1415926535f / 180.0f) * 0.5f;
    float depth = dot(sub(anchor, ctx.eye), ctx.camera.forward);
    if (depth < 1e-4f) depth = 1e-4f;
    return (2.0f * depth * std::tan(half_fov)) / vh;
}

static float arrow_world_size_at_anchor(const DimensionRenderContext &ctx,
                                        const Vec3 &a,
                                        const Vec3 &b,
                                        const Vec3 &anchor) {
    const float world_per_px = world_per_pixel_at_anchor(ctx, anchor);
    const float target = ctx.arrowPixels * world_per_px;
    const float len = std::sqrt(dot(sub(b, a), sub(b, a)));
    const float max_size = len * 0.30f;
    if (max_size <= 1e-8f) return 0.0f;
    float min_size = world_per_px * 1.0f;
    if (min_size > max_size) min_size = max_size;
    return clampf(target, min_size, max_size);
}

static void draw_contour_dimensions(const vicad::ScriptSketchContour &contour,
                                    const DimensionRenderContext &ctx,
                                    float alpha) {
    if (contour.points.size() < 2) return;
    std::vector<Vec2> pts2;
    pts2.reserve(contour.points.size());
    for (const vicad::SceneVec3 &p : contour.points) {
        pts2.push_back({p.x, p.y});
    }
    const float z = contour.points.front().z;
    const Vec2 centroid = contour_centroid_mean(pts2);
    Vec2 bmin = {0.0f, 0.0f}, bmax = {0.0f, 0.0f};
    contour_bbox(pts2, &bmin, &bmax);
    const float bw = bmax.x - bmin.x;
    const float bh = bmax.y - bmin.y;
    const float bdiag = std::sqrt(bw * bw + bh * bh);
    const Vec3 centroid3 = vec3_from_2d(centroid, z);
    const float world_per_px = world_per_pixel_at_anchor(ctx, centroid3);
    float world_scale = (world_per_px * 9.0f) / (float)VICAD_BAKED_PIXEL_SIZE;
    world_scale = clampf(world_scale, world_per_px * 6.0f / (float)VICAD_BAKED_PIXEL_SIZE,
                         world_per_px * 18.0f / (float)VICAD_BAKED_PIXEL_SIZE);
    Vec3 text_right = {1.0f, 0.0f, 0.0f};
    Vec3 text_up = {0.0f, 1.0f, 0.0f};
    Vec3 plane_normal = {0.0f, 0.0f, 1.0f};
    contour_plane_axes(contour, &text_right, &text_up, &plane_normal);
    Vec3 facing_right = text_right;
    Vec3 facing_up = text_up;
    Vec3 facing_normal = plane_normal;
    orient_label_axes_for_camera(ctx.eye, ctx.camera, centroid3, plane_normal, text_right,
                                 &facing_right, &facing_up, &facing_normal);
    const Vec3 label_lift = add(mul(facing_up, world_per_px * 10.0f),
                                mul(facing_normal, world_per_px * 2.0f));

    SketchDimShapeKind kind = SketchDimShapeKind::Polygon;
    float rect_w = 0.0f, rect_h = 0.0f;
    Vec2 c_center = {0.0f, 0.0f};
    float c_radius = 0.0f;
    Vec2 rp_center = {0.0f, 0.0f};
    float rp_radius = 0.0f;
    float rp_side = 0.0f;

    if (classify_rectangle_like(pts2, &rect_w, &rect_h) ||
        classify_rounded_rectangle_like(pts2, &rect_w, &rect_h)) {
        const float maxv = rect_w > rect_h ? rect_w : rect_h;
        const float minv = rect_w > rect_h ? rect_h : rect_w;
        if (maxv > 1e-6f && std::fabs(maxv - minv) / maxv < 0.02f) {
            kind = SketchDimShapeKind::Square;
        } else {
            kind = SketchDimShapeKind::Rectangle;
        }
    } else if (classify_circle_like(pts2, &c_center, &c_radius)) {
        if (c_radius <= 0.2f) {
            kind = SketchDimShapeKind::Point;
        } else {
            kind = SketchDimShapeKind::Circle;
        }
    } else if (classify_regular_polygon_like(pts2, &rp_center, &rp_radius, &rp_side)) {
        kind = SketchDimShapeKind::RegularPolygon;
    } else {
        kind = SketchDimShapeKind::Polygon;
    }

    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    char text[128];

    if (kind == SketchDimShapeKind::Circle || kind == SketchDimShapeKind::Point) {
        Vec2 a2 = {c_center.x - c_radius, c_center.y};
        Vec2 b2 = {c_center.x + c_radius, c_center.y};
        const Vec3 a3 = vec3_from_2d(a2, z);
        const Vec3 b3 = vec3_from_2d(b2, z);
        draw_world_line(a3, b3);
        const Vec3 mid = mul(add(a3, b3), 0.5f);
        draw_dimension_arrowheads_world(a3, b3, facing_normal,
                                        arrow_world_size_at_anchor(ctx, a3, b3, mid));
        if (kind == SketchDimShapeKind::Point) {
            std::snprintf(text, sizeof(text), "P(%.3g, %.3g)", (double)c_center.x, (double)c_center.y);
        } else {
            format_dim(text, sizeof(text), "", c_radius * 2.0f);
        }
        Vec3 dim_dir = normalize(sub(b3, a3));
        if (dot(dim_dir, facing_right) < 0.0f) dim_dir = mul(dim_dir, -1.0f);
        const Vec3 dim_up = normalize(cross(facing_normal, dim_dir));
        draw_dimension_label_world(add(mid, label_lift), dim_dir, dim_up, world_scale, text, 1.0f, 1.0f, 1.0f, alpha, true);
        if (kind == SketchDimShapeKind::Point) {
            const float arm = (bdiag > 0.0f ? bdiag : 1.0f) * 0.06f + 0.04f;
            draw_world_line(vec3_from_2d({c_center.x - arm, c_center.y}, z),
                            vec3_from_2d({c_center.x + arm, c_center.y}, z));
            draw_world_line(vec3_from_2d({c_center.x, c_center.y - arm}, z),
                            vec3_from_2d({c_center.x, c_center.y + arm}, z));
        }
        return;
    }

    if (kind == SketchDimShapeKind::Rectangle || kind == SketchDimShapeKind::Square) {
        const Vec2 p0 = pts2[0];
        const Vec2 p1 = pts2[1];
        const Vec2 p2 = pts2[2];
        const Vec2 e0 = sub2v(p1, p0);
        const Vec2 e1 = sub2v(p2, p1);
        Vec2 n0 = perp2(normalize2(e0));
        Vec2 n1 = perp2(normalize2(e1));
        const Vec2 m0 = mul2(add2(p0, p1), 0.5f);
        const Vec2 m1 = mul2(add2(p1, p2), 0.5f);
        if (dot2(sub2v(m0, centroid), n0) < 0.0f) n0 = mul2(n0, -1.0f);
        if (dot2(sub2v(m1, centroid), n1) < 0.0f) n1 = mul2(n1, -1.0f);
        const float off = (bdiag > 0.0f ? bdiag : 1.0f) * 0.14f + 0.08f;

        const Vec2 w0 = add2(p0, mul2(n0, off));
        const Vec2 w1 = add2(p1, mul2(n0, off));
        const Vec2 h0 = add2(p1, mul2(n1, off));
        const Vec2 h1 = add2(p2, mul2(n1, off));
        const Vec3 w03 = vec3_from_2d(w0, z);
        const Vec3 w13 = vec3_from_2d(w1, z);
        const Vec3 h03 = vec3_from_2d(h0, z);
        const Vec3 h13 = vec3_from_2d(h1, z);
        const Vec3 p03 = vec3_from_2d(p0, z);
        const Vec3 p13 = vec3_from_2d(p1, z);
        const Vec3 p23 = vec3_from_2d(p2, z);

        draw_world_line(w03, w13);
        draw_world_line(h03, h13);
        draw_dimension_arrowheads_world(w03, w13, facing_normal,
                                        arrow_world_size_at_anchor(ctx, w03, w13, mul(add(w03, w13), 0.5f)));
        draw_dimension_arrowheads_world(h03, h13, facing_normal,
                                        arrow_world_size_at_anchor(ctx, h03, h13, mul(add(h03, h13), 0.5f)));
        draw_world_line(p03, w03);
        draw_world_line(p13, w13);
        draw_world_line(p13, h03);
        draw_world_line(p23, h13);

        format_dim(text, sizeof(text), "", rect_w);
        Vec3 w_dir = normalize(sub(w13, w03));
        if (dot(w_dir, facing_right) < 0.0f) w_dir = mul(w_dir, -1.0f);
        const Vec3 w_up = normalize(cross(facing_normal, w_dir));
        draw_dimension_label_world(add(mul(add(w03, w13), 0.5f), label_lift),
                                   w_dir, w_up, world_scale, text, 1.0f, 1.0f, 1.0f, alpha, true);
        format_dim(text, sizeof(text), "", rect_h);
        Vec3 h_dir = normalize(sub(h13, h03));
        if (dot(h_dir, facing_right) < 0.0f) h_dir = mul(h_dir, -1.0f);
        const Vec3 h_up = normalize(cross(facing_normal, h_dir));
        draw_dimension_label_world(add(mul(add(h03, h13), 0.5f), label_lift),
                                   h_dir, h_up, world_scale, text, 1.0f, 1.0f, 1.0f, alpha, true);
        return;
    }

    if (kind == SketchDimShapeKind::RegularPolygon) {
        const size_t n = pts2.size();
        draw_world_line(vec3_from_2d(pts2[0], z), vec3_from_2d(pts2[1], z));
        const float across_flats = 2.0f * rp_radius * std::cos(3.1415926535f / (float)n);
        std::snprintf(text, sizeof(text), "N=%zu  S=%.3g  AF=%.3g", n, (double)rp_side, (double)across_flats);
        draw_dimension_label_world(add(vec3_from_2d(rp_center, z), label_lift),
                                   facing_right, facing_up, world_scale, text, 1.0f, 1.0f, 1.0f, alpha, true);
        return;
    }

    const size_t n = pts2.size();
    if (n <= 8) {
        for (size_t i = 0; i < n; ++i) {
            const Vec2 &a = pts2[i];
            const Vec2 &b = pts2[(i + 1) % n];
            const float e = length2(sub2v(b, a));
            const Vec3 wa = vec3_from_2d(a, z);
            const Vec3 wb = vec3_from_2d(b, z);
            const Vec3 mid = mul(add(wa, wb), 0.5f);
            format_dim(text, sizeof(text), "", e);
            Vec3 e_dir = normalize(sub(wb, wa));
            if (dot(e_dir, facing_right) < 0.0f) e_dir = mul(e_dir, -1.0f);
            const Vec3 e_up = normalize(cross(facing_normal, e_dir));
            draw_dimension_label_world(add(mid, label_lift), e_dir, e_up, world_scale, text,
                                       1.0f, 1.0f, 1.0f, alpha, true);
        }
    } else {
        float perimeter = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            perimeter += length2(sub2v(pts2[(i + 1) % n], pts2[i]));
        }
        const float area = std::fabs(contour_signed_area(pts2));
        std::snprintf(text, sizeof(text), "N=%zu  P=%.3g  A=%.3g", n, (double)perimeter, (double)area);
        draw_dimension_label_world(add(vec3_from_2d(centroid, z), label_lift),
                                   facing_right, facing_up, world_scale, text, 1.0f, 1.0f, 1.0f, alpha, true);
    }
}

static void draw_sketch_dimension_model(const vicad::SketchDimensionModel &model,
                                        float z,
                                        const DimensionRenderContext &ctx,
                                        float alpha) {
    if (model.entities.empty()) return;
    std::vector<Vec2> pts2;
    pts2.reserve(model.logicalVertices.size());
    for (const manifold::vec2 &p : model.logicalVertices) {
        pts2.push_back({(float)p.x, (float)p.y});
    }

    Vec2 bmin = {0.0f, 0.0f};
    Vec2 bmax = {0.0f, 0.0f};
    if (!pts2.empty()) {
        contour_bbox(pts2, &bmin, &bmax);
    } else {
        bmin = {(float)model.anchor.x, (float)model.anchor.y};
        bmax = bmin;
    }
    const float bw = bmax.x - bmin.x;
    const float bh = bmax.y - bmin.y;
    Vec2 centroid = {(float)model.anchor.x, (float)model.anchor.y};
    if (!pts2.empty()) {
        centroid = contour_centroid_mean(pts2);
    }
    const Vec3 centroid3 = vec3_from_2d(centroid, z);
    const float world_per_px = world_per_pixel_at_anchor(ctx, centroid3);
    float world_scale = (world_per_px * 9.0f) / (float)VICAD_BAKED_PIXEL_SIZE;
    world_scale = clampf(world_scale, world_per_px * 6.0f / (float)VICAD_BAKED_PIXEL_SIZE,
                         world_per_px * 18.0f / (float)VICAD_BAKED_PIXEL_SIZE);

    Vec3 facing_right = {1.0f, 0.0f, 0.0f};
    Vec3 facing_up = {0.0f, 1.0f, 0.0f};
    Vec3 facing_normal = {0.0f, 0.0f, 1.0f};
    orient_label_axes_for_camera(ctx.eye, ctx.camera, centroid3, facing_normal, facing_right,
                                 &facing_right, &facing_up, &facing_normal);
    const Vec3 label_lift = add(mul(facing_up, world_per_px * 10.0f),
                                mul(facing_normal, world_per_px * 2.0f));

    char text[128];
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    for (const vicad::SketchDimensionEntity &entity : model.entities) {
        if (entity.kind != vicad::SketchDimensionEntity::Kind::LineDim) continue;
        const Vec3 a3 = vec3_from_2d({(float)entity.line.a.x, (float)entity.line.a.y}, z);
        const Vec3 b3 = vec3_from_2d({(float)entity.line.b.x, (float)entity.line.b.y}, z);
        draw_world_line(a3, b3);
        const Vec3 mid = mul(add(a3, b3), 0.5f);
        const float arrow_size = arrow_world_size_at_anchor(ctx, a3, b3, mid);
        draw_dimension_arrowheads_world(a3, b3, facing_normal, arrow_size);
        format_dim(text, sizeof(text), "", (float)entity.line.value);
        Vec3 dim_dir = normalize(sub(b3, a3));
        if (dot(dim_dir, facing_right) < 0.0f) dim_dir = mul(dim_dir, -1.0f);
        const Vec3 dim_up = normalize(cross(facing_normal, dim_dir));
        draw_dimension_label_world(add(mid, label_lift), dim_dir, dim_up, world_scale, text,
                                   1.0f, 1.0f, 1.0f, alpha, true);
    }
}

static void draw_script_sketch_dimensions(const std::vector<vicad::ScriptSceneObject> &scene,
                                          int selected_object_index,
                                          const DimensionRenderContext &ctx,
                                          bool show_dimensions) {
    if (!show_dimensions) return;
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glLineWidth(1.5f);
    for (size_t scene_index = 0; scene_index < scene.size(); ++scene_index) {
        const vicad::ScriptSceneObject &obj = scene[scene_index];
        if (!scene_object_is_sketch(obj)) continue;
        if (obj.sketchContours.empty()) continue;
        const bool selected = ((int)scene_index == selected_object_index);
        const float alpha = selected ? 1.0f : 0.30f;
        glColor4f(1.0f, 1.0f, 1.0f, alpha);

        size_t largest_idx = 0;
        float largest_area = -1.0f;
        for (size_t i = 0; i < obj.sketchContours.size(); ++i) {
            const vicad::ScriptSketchContour &c = obj.sketchContours[i];
            if (c.points.size() < 3) continue;
            std::vector<Vec2> pts;
            pts.reserve(c.points.size());
            for (const vicad::SceneVec3 &p : c.points) pts.push_back({p.x, p.y});
            const float a = std::fabs(contour_signed_area(pts));
            if (a > largest_area) {
                largest_area = a;
                largest_idx = i;
            }
        }
        if (obj.sketchDims.has_value()) {
            float z = 0.0f;
            if (!obj.sketchContours.empty() && !obj.sketchContours.front().points.empty()) {
                z = obj.sketchContours.front().points.front().z;
            }
            draw_sketch_dimension_model(*obj.sketchDims, z, ctx, alpha);
        } else {
            draw_contour_dimensions(obj.sketchContours[largest_idx], ctx, alpha);
        }
    }

    glLineWidth(1.0f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
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

static void window_mouse_to_pixel(i32 mouse_x, i32 mouse_y,
                                  i32 window_w, i32 window_h,
                                  i32 pixel_w, i32 pixel_h,
                                  i32 *out_x, i32 *out_y) {
    i32 x = mouse_x;
    i32 y = mouse_y;
    if (window_w > 0 && window_h > 0 && pixel_w > 0 && pixel_h > 0) {
        const float sx = (float)pixel_w / (float)window_w;
        const float sy = (float)pixel_h / (float)window_h;
        x = (i32)std::floor(((float)mouse_x + 0.5f) * sx);
        y = (i32)std::floor(((float)mouse_y + 0.5f) * sy);
    }
    if (pixel_w > 0) {
        if (x < 0) x = 0;
        if (x >= pixel_w) x = pixel_w - 1;
    }
    if (pixel_h > 0) {
        if (y < 0) y = 0;
        if (y >= pixel_h) y = pixel_h - 1;
    }
    *out_x = x;
    *out_y = y;
}

static void draw_pick_debug_overlay(i32 pixel_w, i32 pixel_h,
                                    i32 window_w, i32 window_h,
                                    i32 raw_mouse_x, i32 raw_mouse_y,
                                    i32 pick_mouse_x, i32 pick_mouse_y) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)pixel_w, (double)pixel_h, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto draw_cross = [](float x, float y, float half, float r, float g, float b, float a) {
        glColor4f(r, g, b, a);
        glBegin(GL_LINES);
        glVertex2f(x - half, y);
        glVertex2f(x + half, y);
        glVertex2f(x, y - half);
        glVertex2f(x, y + half);
        glEnd();
    };

    float raw_px_x = (float)raw_mouse_x;
    float raw_px_y = (float)raw_mouse_y;
    if (window_w > 0 && window_h > 0 && pixel_w > 0 && pixel_h > 0) {
        const float sx = (float)pixel_w / (float)window_w;
        const float sy = (float)pixel_h / (float)window_h;
        raw_px_x = ((float)raw_mouse_x + 0.5f) * sx;
        raw_px_y = ((float)raw_mouse_y + 0.5f) * sy;
    }

    // Magenta: raw window-space coords drawn directly in pixel space (shows HiDPI scale mismatch).
    draw_cross((float)raw_mouse_x, (float)raw_mouse_y, 7.0f, 0.95f, 0.20f, 0.95f, 0.75f);
    // Red: raw mouse converted to pixel space.
    draw_cross(raw_px_x, raw_px_y, 10.0f, 1.0f, 0.25f, 0.25f, 0.95f);
    // Cyan: coordinate actually used for ray picking (should match red).
    draw_cross((float)pick_mouse_x, (float)pick_mouse_y, 12.0f, 0.22f, 0.92f, 1.0f, 0.95f);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

static bool ray_intersect_triangle(const Vec3 &orig, const Vec3 &dir,
                                   const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                                   float *out_t) {
    const Vec3 e1 = sub(p1, p0);
    const Vec3 e2 = sub(p2, p0);
    const Vec3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-7f) return false;
    const float inv_det = 1.0f / det;

    const Vec3 tvec = sub(orig, p0);
    const float u = dot(tvec, p) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;

    const Vec3 q = cross(tvec, e1);
    const float v = dot(dir, q) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;

    const float t = dot(e2, q) * inv_det;
    if (t <= 1e-5f) return false;
    if (out_t) *out_t = t;
    return true;
}

static bool ray_mesh_hit_t(const manifold::MeshGL &mesh,
                           const Vec3 &ray_origin, const Vec3 &ray_dir,
                           float *out_t) {
    const size_t tri_count = mesh.NumTri();
    if (tri_count == 0) return false;

    bool hit = false;
    float best_t = 1e30f;
    for (size_t tri = 0; tri < tri_count; ++tri) {
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

        float t = 0.0f;
        if (!ray_intersect_triangle(ray_origin, ray_dir, p0, p1, p2, &t)) continue;
        if (t >= best_t) continue;
        best_t = t;
        hit = true;
    }

    if (hit && out_t) *out_t = best_t;
    return hit;
}

static bool ray_hits_mesh(const manifold::MeshGL &mesh,
                          const Vec3 &ray_origin, const Vec3 &ray_dir) {
    return ray_mesh_hit_t(mesh, ray_origin, ray_dir, nullptr);
}

static bool ray_aabb_hit_t(const Vec3 &ray_origin, const Vec3 &ray_dir,
                           const Vec3 &bmin, const Vec3 &bmax, float *out_t) {
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
    if (tmax < 0.0f) return false;
    if (out_t) *out_t = tmin >= 0.0f ? tmin : tmax;
    return true;
}

static Vec3 scene_vec3_to_vec3(const vicad::SceneVec3 &v) {
    return {v.x, v.y, v.z};
}

static int pick_scene_object_by_ray(const std::vector<vicad::ScriptSceneObject> &scene,
                                    const Vec3 &eye, const Vec3 &ray_dir) {
    float best_t = 1e30f;
    int best = -1;
    for (size_t i = 0; i < scene.size(); ++i) {
        float broad_t = 0.0f;
        if (!ray_aabb_hit_t(eye, ray_dir,
                            scene_vec3_to_vec3(scene[i].bmin),
                            scene_vec3_to_vec3(scene[i].bmax),
                            &broad_t)) {
            continue;
        }
        float hit_t = broad_t;
        if (scene_object_is_manifold(scene[i])) {
            if (!ray_mesh_hit_t(scene[i].mesh, eye, ray_dir, &hit_t)) {
                continue;
            }
        } else if (!scene_object_is_sketch(scene[i])) {
            continue;
        }
        if (hit_t < best_t) {
            best_t = hit_t;
            best = (int)i;
        }
    }
    return best;
}

static void draw_orientation_cube(const CameraBasis &basis, i32 width, i32 height, int top_offset_px, float hud_scale) {
    const float cube_scale = clampf(hud_scale / kHudLegacyScale, 0.5f, 2.0f);
    int size = (int)std::lround((float)(width < height ? width : height) / 5.0f * cube_scale);
    const int min_size = (int)std::lround(96.0f * cube_scale);
    const int max_size = (int)std::lround(180.0f * cube_scale);
    if (size < min_size) size = min_size;
    if (size > max_size) size = max_size;
    const int pad = (int)std::lround(16.0f * cube_scale);
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

static bool point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < (rx + rw) && y >= ry && y < (ry + rh);
}

static void draw_text_baked_rgba(float x, float y, float font_px, const char *text,
                                 float r, float g, float b, float a) {
    if (!text || !text[0]) return;
    if (font_px <= 0.0f) return;
    if (!ensure_baked_font_texture()) return;

    const float scale = font_px / (float)VICAD_BAKED_PIXEL_SIZE;
    const float line_height = (float)VICAD_BAKED_LINE_HEIGHT * scale;
    const float shadow_px = clampf(font_px * 0.055f, 1.0f, 2.0f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_baked_font.texture);
    auto draw_pass = [&](float x_off, float y_off, float cr, float cg, float cb, float ca) {
        float pen_x = x + x_off;
        float baseline = y + y_off + (float)VICAD_BAKED_ASCENDER * scale;
        glColor4f(cr, cg, cb, ca);
        glBegin(GL_QUADS);
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            if (*p == '\n') {
                pen_x = x + x_off;
                baseline += line_height;
                continue;
            }

            const VicadBakedGlyph *glyph = glyph_for_char(*p);
            if (glyph->w > 0 && glyph->h > 0) {
                const float x0 = pen_x + (float)glyph->bearing_x * scale;
                const float y0 = baseline - (float)glyph->bearing_y * scale;
                const float x1 = x0 + (float)glyph->w * scale;
                const float y1 = y0 + (float)glyph->h * scale;
                const float u0 = (float)glyph->x / (float)VICAD_BAKED_ATLAS_WIDTH;
                const float v0 = (float)glyph->y / (float)VICAD_BAKED_ATLAS_HEIGHT;
                const float u1 = (float)(glyph->x + glyph->w) / (float)VICAD_BAKED_ATLAS_WIDTH;
                const float v1 = (float)(glyph->y + glyph->h) / (float)VICAD_BAKED_ATLAS_HEIGHT;

                glTexCoord2f(u0, v0); glVertex2f(x0, y0);
                glTexCoord2f(u1, v0); glVertex2f(x1, y0);
                glTexCoord2f(u1, v1); glVertex2f(x1, y1);
                glTexCoord2f(u0, v1); glVertex2f(x0, y1);
            }
            pen_x += (float)glyph->advance * scale;
        }
        glEnd();
    };

    draw_pass(shadow_px, shadow_px, 0.02f, 0.03f, 0.04f, a * kTextShadowAlphaScale);
    draw_pass(0.0f, 0.0f, r, g, b, a);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

static void draw_text_baked_world(const Vec3 &origin,
                                  const Vec3 &axis_right,
                                  const Vec3 &axis_up,
                                  float world_scale, const char *text,
                                  float r, float g, float b, float a) {
    if (!text || !text[0]) return;
    if (world_scale <= 0.0f) return;
    if (!ensure_baked_font_texture()) return;

    const float line_height = (float)VICAD_BAKED_LINE_HEIGHT * world_scale;
    const Vec3 down = mul(axis_up, -1.0f);
    const float shadow_world = world_scale * 1.35f;
    const Vec3 shadow_origin = add(origin, add(mul(axis_right, shadow_world), mul(down, shadow_world)));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_baked_font.texture);
    auto draw_pass = [&](const Vec3 &pass_origin, float cr, float cg, float cb, float ca) {
        float pen_x = 0.0f;
        float baseline = (float)VICAD_BAKED_ASCENDER * world_scale;
        glColor4f(cr, cg, cb, ca);
        glBegin(GL_QUADS);
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            if (*p == '\n') {
                pen_x = 0.0f;
                baseline += line_height;
                continue;
            }

            const VicadBakedGlyph *glyph = glyph_for_char(*p);
            if (glyph->w > 0 && glyph->h > 0) {
                const float x0 = pen_x + (float)glyph->bearing_x * world_scale;
                const float y0 = baseline - (float)glyph->bearing_y * world_scale;
                const float x1 = x0 + (float)glyph->w * world_scale;
                const float y1 = y0 + (float)glyph->h * world_scale;
                const float u0 = (float)glyph->x / (float)VICAD_BAKED_ATLAS_WIDTH;
                const float v0 = (float)glyph->y / (float)VICAD_BAKED_ATLAS_HEIGHT;
                const float u1 = (float)(glyph->x + glyph->w) / (float)VICAD_BAKED_ATLAS_WIDTH;
                const float v1 = (float)(glyph->y + glyph->h) / (float)VICAD_BAKED_ATLAS_HEIGHT;

                const Vec3 p00 = add(add(pass_origin, mul(axis_right, x0)), mul(down, y0));
                const Vec3 p10 = add(add(pass_origin, mul(axis_right, x1)), mul(down, y0));
                const Vec3 p11 = add(add(pass_origin, mul(axis_right, x1)), mul(down, y1));
                const Vec3 p01 = add(add(pass_origin, mul(axis_right, x0)), mul(down, y1));

                glTexCoord2f(u0, v0); glVertex3f(p00.x, p00.y, p00.z);
                glTexCoord2f(u1, v0); glVertex3f(p10.x, p10.y, p10.z);
                glTexCoord2f(u1, v1); glVertex3f(p11.x, p11.y, p11.z);
                glTexCoord2f(u0, v1); glVertex3f(p01.x, p01.y, p01.z);
            }
            pen_x += (float)glyph->advance * world_scale;
        }
        glEnd();
    };

    draw_pass(shadow_origin, 0.02f, 0.03f, 0.04f, a * kTextShadowAlphaScale);
    draw_pass(origin, r, g, b, a);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

static void draw_text_5x7(float x, float y, float scale, const char *text, float r, float g, float b) {
    draw_text_baked_rgba(x, y, scale * 7.0f, text, r, g, b, 1.0f);
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

static bool export_mesh_to_3mf_native(const char *out_path, const manifold::MeshGL &mesh, std::string *error) {
    if (!out_path || out_path[0] == '\0') {
        if (error) *error = "Output path is empty.";
        return false;
    }
    if (mesh.numProp < 3 || mesh.vertProperties.empty() || mesh.triVerts.empty()) {
        if (error) *error = "Mesh is empty; nothing to export.";
        return false;
    }
    try {
        manifold::ExportMesh(out_path, mesh, manifold::ExportOptions{});
    } catch (const std::exception &e) {
        if (error) *error = std::string("3MF export failed: ") + e.what();
        return false;
    } catch (...) {
        if (error) *error = "3MF export failed with unknown exception.";
        return false;
    }
    return true;
}

static bool export_script_scene_3mf(vicad::ScriptWorkerClient *worker_client,
                                    const char *script_path,
                                    const char *out_path,
                                    std::string *error) {
    if (!worker_client) {
        if (error) *error = "Invalid worker client.";
        return false;
    }
    if (!script_path || script_path[0] == '\0') {
        if (error) *error = "Script path is empty.";
        return false;
    }
    std::vector<vicad::ScriptSceneObject> scene_objects;
    vicad::ReplayLodPolicy lod_policy = {};
    lod_policy.profile = vicad::LodProfile::Export3MF;
    if (!worker_client->ExecuteScriptScene(script_path, &scene_objects, error, lod_policy)) {
        return false;
    }
    std::vector<manifold::Manifold> parts;
    parts.reserve(scene_objects.size());
    for (const vicad::ScriptSceneObject &obj : scene_objects) {
        if (obj.kind == vicad::ScriptSceneObjectKind::Manifold) {
            parts.push_back(obj.manifold);
        }
    }
    if (parts.empty()) {
        if (error) *error = "Worker returned no manifold scene objects.";
        return false;
    }
    manifold::Manifold merged = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
    if (merged.Status() != manifold::Manifold::Error::NoError) {
        if (error) *error = "Failed to merge scene objects for mesh export.";
        return false;
    }
    manifold::MeshGL export_mesh = merged.GetMeshGL();
    return export_mesh_to_3mf_native(out_path, export_mesh, error);
}

static Clay_RenderCommandArray build_clay_ui(i32 width, i32 height, bool script_ok,
                                             const std::string &script_error,
                                             size_t tri_count, size_t vert_count,
                                             bool selected,
                                             size_t scene_object_count,
                                             size_t scene_sketch_count,
                                             int selected_object_index,
                                             int hovered_object_index,
                                             uint64_t selected_object_id,
                                             uint64_t hovered_object_id,
                                             bool show_sketch_dimensions,
                                             const vicad::ScriptSceneObject *info_object,
                                             float hud_scale,
                                             const FaceSelectState &face_select,
                                             const EdgeSelectState &edge_select,
                                             const std::string &export_status_line,
                                             bool export_status_ok) {
    const float hud = vicad_render_ui::ClampHudScale(hud_scale);
    const int root_pad = (int)std::lround(16.0f * hud);
    int panel_w = (int)std::lround(320.0f * hud);
    if (panel_w > width - root_pad * 2) panel_w = width - root_pad * 2;
    if (panel_w < 320) panel_w = 320;
    const int header_h = (int)std::lround(28.0f * hud);
    const int header_pad = (int)std::lround(8.0f * hud);
    const int body_pad = (int)std::lround(10.0f * hud);
    const int body_gap = (int)std::lround(6.0f * hud);
    const int title_font = (int)std::lround(17.0f * hud);
    const int body_font = (int)std::lround(15.0f * hud);
    const int small_font = (int)std::lround(14.0f * hud);
    const int stack_gap = (int)std::lround(8.0f * hud);

    clay_init(width, height);
    Clay_SetCurrentContext(g_ui.ctx);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
    Clay_BeginLayout();

    char mesh_line[96];
    char selected_line[96];
    char object_count_line[96];
    char sketch_count_line[96];
    char object_pick_line[160];
    char sel_mode_line[96];
    char edge_mode_line[96];
    char edge_count_line[128];
    char edge_hover_line[96];
    char face_mode_line[96];
    char face_region_line[96];
    char face_hover_line[96];
    char face_type_line[128];
    char dims_toggle_line[64];
    std::snprintf(mesh_line, sizeof(mesh_line), "MESH: %zu TRI  %zu VERT", tri_count, vert_count);
    std::snprintf(selected_line, sizeof(selected_line), "SELECTED: %s", selected ? "ON" : "OFF");
    std::snprintf(object_count_line, sizeof(object_count_line), "SCENE OBJECTS: %zu", scene_object_count);
    std::snprintf(sketch_count_line, sizeof(sketch_count_line), "SKETCH LAYERS: %zu", scene_sketch_count);
    std::snprintf(object_pick_line, sizeof(object_pick_line), "OBJ H/S IDX: %d / %d  ID: %llx / %llx",
                  hovered_object_index, selected_object_index,
                  (unsigned long long)hovered_object_id,
                  (unsigned long long)selected_object_id);
    const char *mode_name = edge_select.enabled ? "EDGE" : (face_select.enabled ? "FACE" : "OBJECT");
    std::snprintf(sel_mode_line, sizeof(sel_mode_line), "SEL MODE: %s [E/F]", mode_name);
    std::snprintf(edge_mode_line, sizeof(edge_mode_line), "EDGE SEL: %s [E]", edge_select.enabled ? "ON" : "OFF");
    std::snprintf(edge_count_line, sizeof(edge_count_line),
                  "EDGES S/B/NM/SIL: %zu/%zu/%zu/%zu  ANGLE: %.1f",
                  edge_select.edges.sharpEdgeIndices.size(),
                  edge_select.edges.boundaryEdgeIndices.size(),
                  edge_select.edges.nonManifoldEdgeIndices.size(),
                  edge_select.silhouette.silhouetteEdgeIndices.size(),
                  edge_select.sharpAngleDeg);
    std::snprintf(edge_hover_line, sizeof(edge_hover_line), "H/S EDGE: %d/%d  CHAIN: %d/%d",
                  edge_select.hoveredEdge, edge_select.selectedEdge,
                  edge_select.hoveredChain, edge_select.selectedChain);
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
    std::snprintf(dims_toggle_line, sizeof(dims_toggle_line), "SKETCH DIMS: %s [D]",
                  show_sketch_dimensions ? "ON" : "OFF");

    const std::string one_line_error = script_error.empty() ? "NO ERROR TEXT" :
        script_error.substr(0, script_error.find('\n'));

    size_t slot = 0;
    Clay_String mesh_line_s = ui_text_slot(slot++, mesh_line);
    Clay_String selected_line_s = ui_text_slot(slot++, selected_line);
    Clay_String object_count_line_s = ui_text_slot(slot++, object_count_line);
    Clay_String sketch_count_line_s = ui_text_slot(slot++, sketch_count_line);
    Clay_String object_pick_line_s = ui_text_slot(slot++, object_pick_line);
    Clay_String sel_mode_line_s = ui_text_slot(slot++, sel_mode_line);
    Clay_String edge_mode_line_s = ui_text_slot(slot++, edge_mode_line);
    Clay_String edge_count_line_s = ui_text_slot(slot++, edge_count_line);
    Clay_String edge_hover_line_s = ui_text_slot(slot++, edge_hover_line);
    Clay_String face_mode_line_s = ui_text_slot(slot++, face_mode_line);
    Clay_String face_region_line_s = ui_text_slot(slot++, face_region_line);
    Clay_String face_hover_line_s = ui_text_slot(slot++, face_hover_line);
    Clay_String face_type_line_s = ui_text_slot(slot++, face_type_line);
    Clay_String dims_toggle_line_s = ui_text_slot(slot++, dims_toggle_line);
    Clay_String err_line_s = ui_text_slot(slot++, one_line_error);
    Clay_String export_status_line_s = ui_text_slot(slot++, export_status_line);

    std::vector<std::string> info_lines;
    info_lines.reserve(24);
    if (!info_object) {
        info_lines.push_back("No selection");
    } else {
        char tmp[256];
        std::snprintf(tmp, sizeof(tmp), "Name: %s", info_object->name.c_str());
        info_lines.push_back(tmp);
        std::snprintf(tmp, sizeof(tmp), "Kind: %s", vicad_render_scene::SceneObjectKindName(info_object->kind));
        info_lines.push_back(tmp);
        std::snprintf(tmp, sizeof(tmp), "Object ID: %llx", (unsigned long long)info_object->objectId);
        info_lines.push_back(tmp);
        std::snprintf(tmp, sizeof(tmp), "Root kind/id: %u / %u", info_object->rootKind, info_object->rootId);
        info_lines.push_back(tmp);
        if (info_object->sketchDims.has_value()) {
            const vicad::SketchDimensionModel &dims = *info_object->sketchDims;
            std::snprintf(tmp, sizeof(tmp), "Sketch primitive: %s", vicad::SketchPrimitiveKindName(dims.primitive));
            info_lines.push_back(tmp);
            if (dims.hasRectSize) {
                std::snprintf(tmp, sizeof(tmp), "Rect W/H: %.4g / %.4g", dims.rectWidth, dims.rectHeight);
                info_lines.push_back(tmp);
            }
            if (dims.polygonSides > 0) {
                std::snprintf(tmp, sizeof(tmp), "Polygon edges: %u", dims.polygonSides);
                info_lines.push_back(tmp);
            }
            if (dims.hasFillet) {
                std::snprintf(tmp, sizeof(tmp), "Fillet radius: %.4g", dims.filletRadius);
                info_lines.push_back(tmp);
            }
        }
        info_lines.push_back("Ops:");
        if (info_object->opTrace.empty()) {
            info_lines.push_back("  (none)");
        } else {
            for (size_t i = 0; i < info_object->opTrace.size(); ++i) {
                std::snprintf(tmp, sizeof(tmp), "  %zu. %s", i + 1, format_op_trace_entry(info_object->opTrace[i]).c_str());
                info_lines.push_back(tmp);
            }
        }
    }

    CLAY(CLAY_ID("StatusRoot"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .padding = CLAY_PADDING_ALL((uint16_t)root_pad),
            .childGap = (uint16_t)stack_gap,
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
                    CLAY_TEXT(mesh_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {230, 238, 248, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(selected_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = selected ? (Clay_Color){255, 230, 70, 255} : (Clay_Color){190, 200, 212, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(object_count_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {184, 212, 252, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sketch_count_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {150, 224, 255, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(object_pick_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {198, 218, 246, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(sel_mode_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {170, 210, 255, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(dims_toggle_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {210, 236, 255, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY_TEXT(edge_mode_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = edge_select.enabled ? (Clay_Color){138, 196, 255, 255} : (Clay_Color){190, 200, 212, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    if (edge_select.enabled) {
                        CLAY_TEXT(edge_count_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {210, 222, 238, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(edge_hover_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {210, 222, 238, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("LCLICK PICK EDGE"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("+/- ADJUST SHARP ANGLE"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    CLAY_TEXT(face_mode_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = face_select.enabled ? (Clay_Color){138, 196, 255, 255} : (Clay_Color){190, 200, 212, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    if (face_select.enabled) {
                        CLAY_TEXT(face_region_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {210, 222, 238, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(face_hover_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {210, 222, 238, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(face_type_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {210, 222, 238, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("LCLICK PICK DETECTED FACE"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(CLAY_STRING("+/- ADJUST FACE ANGLE"),
                                  CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {205, 214, 230, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    CLAY_TEXT(CLAY_STRING("SCRIPT SKETCHES: vicad.addSketch(CrossSection, opts)"),
                              CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {190, 216, 245, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    if (!script_ok) {
                        CLAY_TEXT(CLAY_STRING("LAST ERROR:"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {255, 170, 170, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_TEXT(err_line_s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {255, 205, 205, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                }
            }
        }

        CLAY(CLAY_ID("InfoPanel"), {
            .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = {.width = CLAY_SIZING_FIXED((float)panel_w), .height = CLAY_SIZING_FIT(0)},
            },
            .backgroundColor = {20, 25, 33, 235},
            .border = {.color = {90, 100, 118, 255}, .width = {.left = 1, .right = 1, .top = 1, .bottom = 1}}
        }) {
            CLAY(CLAY_ID("InfoHeader"), {
                .layout = {
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED((float)header_h)},
                    .padding = {(uint16_t)header_pad, (uint16_t)header_pad, (uint16_t)(header_pad * 3 / 4), (uint16_t)(header_pad * 3 / 4)},
                    .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = {36, 44, 54, 250},
            }) {
                CLAY_TEXT(CLAY_STRING("INFO"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)title_font, .textColor = {245, 248, 252, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            CLAY(CLAY_ID_LOCAL("InfoBody"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                    .padding = CLAY_PADDING_ALL((uint16_t)body_pad),
                    .childGap = (uint16_t)body_gap,
                }
            }) {
                for (size_t i = 0; i < info_lines.size(); ++i) {
                    Clay_String s = ui_text_slot(slot++, info_lines[i]);
                    CLAY_TEXT(s, CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font, .textColor = {210, 222, 238, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }

        CLAY(CLAY_ID("ExportPanel"), {
            .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = {.width = CLAY_SIZING_FIXED((float)panel_w), .height = CLAY_SIZING_FIT(0)},
            },
            .backgroundColor = {20, 25, 33, 235},
            .border = {.color = {90, 100, 118, 255}, .width = {.left = 1, .right = 1, .top = 1, .bottom = 1}}
        }) {
            CLAY(CLAY_ID("ExportHeader"), {
                .layout = {
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED((float)header_h)},
                    .padding = {(uint16_t)header_pad, (uint16_t)header_pad, (uint16_t)(header_pad * 3 / 4), (uint16_t)(header_pad * 3 / 4)},
                    .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = {36, 44, 54, 250},
            }) {
                CLAY_TEXT(CLAY_STRING("EXPORT"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)title_font, .textColor = {245, 248, 252, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            CLAY(CLAY_ID_LOCAL("ExportBody"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                    .padding = CLAY_PADDING_ALL((uint16_t)body_pad),
                    .childGap = (uint16_t)body_gap,
                }
            }) {
                CLAY(CLAY_ID("Export3mfButton"), {
                    .layout = {
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED((float)header_h)},
                        .padding = CLAY_PADDING_ALL((uint16_t)header_pad),
                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                    },
                    .backgroundColor = {52, 84, 132, 255},
                    .border = {.color = {120, 158, 214, 255}, .width = {.left = 1, .right = 1, .top = 1, .bottom = 1}}
                }) {
                    CLAY_TEXT(CLAY_STRING("Export .3mf"), CLAY_TEXT_CONFIG({.fontSize = (uint16_t)body_font, .textColor = {245, 248, 252, 255}, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
                CLAY_TEXT(export_status_line_s,
                          CLAY_TEXT_CONFIG({.fontSize = (uint16_t)small_font,
                                            .textColor = export_status_ok ? (Clay_Color){130, 230, 130, 255}
                                                                          : (Clay_Color){255, 168, 140, 255},
                                            .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            }
        }
    }

    Clay_RenderCommandArray cmds = Clay_EndLayout();
    g_ui.panelData = Clay_GetElementData(CLAY_ID("StatusPanel"));
    g_ui.headerData = Clay_GetElementData(CLAY_ID("StatusHeader"));
    g_ui.infoPanelData = Clay_GetElementData(CLAY_ID("InfoPanel"));
    g_ui.exportPanelData = Clay_GetElementData(CLAY_ID("ExportPanel"));
    g_ui.exportButtonData = Clay_GetElementData(CLAY_ID("Export3mfButton"));
    return cmds;
}

int main() {
    static RGFW_glHints gl_hints = RGFW_DEFAULT_GL_HINTS;
    gl_hints.samples = kRequestedMsaaSamples;
    RGFW_setGlobalHints_OpenGL(&gl_hints);

    const bool feature_detection_enabled = false;
    manifold::Manifold fallback = manifold::Manifold::Cube(manifold::vec3(1.0), true);
    manifold::MeshGL base_mesh = fallback.GetMeshGL();
    manifold::MeshGL mesh = base_mesh;
    Vec3 mesh_bmin = {-0.5f, -0.5f, -0.5f};
    Vec3 mesh_bmax = {0.5f, 0.5f, 0.5f};
    (void)compute_mesh_bounds(mesh, &mesh_bmin, &mesh_bmax);

    const char *script_path = "myobject.vicad.ts";
    vicad::ScriptWorkerClient worker_client;
    bool ipc_start_failed = false;
    std::vector<vicad::ScriptSceneObject> script_scene;

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
    glEnable(GL_MULTISAMPLE);
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
    float ui_scale = 1.0f;
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
    bool show_sketch_dimensions = true;
    bool object_selected = false;
    int selected_object_index = -1;
    int hovered_object_index = -1;
    FaceSelectState face_select = {
        false,  // enabled
        true,   // dirty
        22.0f,  // angleThresholdDeg
        -1,     // hoveredRegion
        -1,     // selectedRegion
        {},     // faces
    };
    EdgeSelectState edge_select = {
        false,  // enabled
        true,   // dirtyTopology
        true,   // dirtySilhouette
        30.0f,  // sharpAngleDeg
        -1,     // hoveredEdge
        -1,     // selectedEdge
        -1,     // hoveredChain
        -1,     // selectedChain
        {},     // edges
        {},     // silhouette
    };
    float ui_scroll_x = 0.0f;
    float ui_scroll_y = 0.0f;
    i32 last_mouse_x = 0;
    i32 last_mouse_y = 0;
    bool have_last_mouse = false;
    bool pick_debug_overlay = true;
    std::string export_status_line = "Ready (uses Export3MF LOD profile)";
    bool export_status_ok = true;

    while (!RGFW_window_shouldClose(win)) {
        const long long mt = file_mtime_ns(script_path);
        if (mt >= 0 && mt != last_script_mtime) {
            last_script_mtime = mt;
            manifold::MeshGL next_mesh;
            Vec3 next_bmin = {0.0f, 0.0f, 0.0f};
            Vec3 next_bmax = {0.0f, 0.0f, 0.0f};
            std::string err;

            bool loaded = false;
            std::vector<vicad::ScriptSceneObject> next_scene;
            vicad::ReplayLodPolicy lod_policy = {};
            lod_policy.profile = vicad::LodProfile::Model;
            loaded = worker_client.ExecuteScriptScene(script_path, &next_scene,
                                                      &err, lod_policy);
            if (loaded) {
                script_scene = std::move(next_scene);
                std::vector<manifold::Manifold> parts;
                parts.reserve(script_scene.size());
                for (const vicad::ScriptSceneObject &obj : script_scene) {
                    if (scene_object_is_manifold(obj)) {
                        parts.push_back(obj.manifold);
                    }
                }
                if (!parts.empty()) {
                    manifold::Manifold merged = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
                    if (merged.Status() != manifold::Manifold::Error::NoError) {
                        loaded = false;
                        err = std::string("Scene merge failed: ") + manifold_error_string(merged.Status());
                    } else {
                        next_mesh = merged.GetMeshGL();
                        if (!compute_mesh_bounds(next_mesh, &next_bmin, &next_bmax)) {
                            loaded = false;
                            err = "Merged scene mesh has no valid bounds.";
                        }
                    }
                } else {
                    next_mesh.numProp = 3;
                    if (!compute_scene_bounds(script_scene, &next_bmin, &next_bmax)) {
                        loaded = false;
                        err = "Scene has no manifold or sketch geometry to visualize.";
                    }
                }
            }
            if (!loaded && !worker_client.started()) {
                ipc_start_failed = true;
                std::fprintf(stderr, "[vicad] IPC worker startup failed: %s\n", err.c_str());
            }

            if (loaded) {
                base_mesh = std::move(next_mesh);
                mesh = base_mesh;
                bool have_mesh_bounds = compute_mesh_bounds(mesh, &mesh_bmin, &mesh_bmax);
                Vec3 scene_bmin = {0.0f, 0.0f, 0.0f};
                Vec3 scene_bmax = {0.0f, 0.0f, 0.0f};
                const bool have_scene_bounds = compute_scene_bounds(script_scene, &scene_bmin, &scene_bmax);
                if (!have_mesh_bounds && have_scene_bounds) {
                    mesh_bmin = scene_bmin;
                    mesh_bmax = scene_bmax;
                    have_mesh_bounds = true;
                } else if (have_mesh_bounds && have_scene_bounds) {
                    if (scene_bmin.x < mesh_bmin.x) mesh_bmin.x = scene_bmin.x;
                    if (scene_bmin.y < mesh_bmin.y) mesh_bmin.y = scene_bmin.y;
                    if (scene_bmin.z < mesh_bmin.z) mesh_bmin.z = scene_bmin.z;
                    if (scene_bmax.x > mesh_bmax.x) mesh_bmax.x = scene_bmax.x;
                    if (scene_bmax.y > mesh_bmax.y) mesh_bmax.y = scene_bmax.y;
                    if (scene_bmax.z > mesh_bmax.z) mesh_bmax.z = scene_bmax.z;
                } else if (!have_mesh_bounds) {
                    mesh_bmin = next_bmin;
                    mesh_bmax = next_bmax;
                }
                object_selected = false;
                selected_object_index = -1;
                hovered_object_index = -1;
                face_select.dirty = true;
                face_select.hoveredRegion = -1;
                face_select.selectedRegion = -1;
                edge_select.dirtyTopology = true;
                edge_select.dirtySilhouette = true;
                edge_select.hoveredEdge = -1;
                edge_select.selectedEdge = -1;
                edge_select.hoveredChain = -1;
                edge_select.selectedChain = -1;
                script_error.clear();
                std::fprintf(stderr, "[vicad] script loaded: %s (ipc)\n", script_path);
            } else {
                if (ipc_start_failed && err.empty()) {
                    err = "IPC startup failed.";
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
                if (feature_detection_enabled && key == RGFW_e) {
                    edge_select.enabled = !edge_select.enabled;
                    if (edge_select.enabled) {
                        face_select.enabled = false;
                        face_select.hoveredRegion = -1;
                        face_select.selectedRegion = -1;
                        edge_select.dirtyTopology = true;
                        edge_select.dirtySilhouette = true;
                    } else {
                        edge_select.hoveredEdge = -1;
                        edge_select.selectedEdge = -1;
                        edge_select.hoveredChain = -1;
                        edge_select.selectedChain = -1;
                    }
                } else if (feature_detection_enabled && key == RGFW_f &&
                           (RGFW_window_isKeyDown(win, RGFW_shiftL) ||
                            RGFW_window_isKeyDown(win, RGFW_shiftR))) {
                    face_select.enabled = !face_select.enabled;
                    if (face_select.enabled) {
                        edge_select.enabled = false;
                        edge_select.hoveredEdge = -1;
                        edge_select.hoveredChain = -1;
                        face_select.dirty = true;
                    } else {
                        face_select.hoveredRegion = -1;
                        face_select.selectedRegion = -1;
                    }
                } else if (edge_select.enabled &&
                           (key == RGFW_equal || key == RGFW_kpPlus)) {
                    edge_select.sharpAngleDeg = clampf(edge_select.sharpAngleDeg + 1.0f, 1.0f, 89.0f);
                    edge_select.dirtyTopology = true;
                    edge_select.selectedEdge = -1;
                    edge_select.selectedChain = -1;
                } else if (edge_select.enabled &&
                           (key == RGFW_minus || key == RGFW_kpMinus)) {
                    edge_select.sharpAngleDeg = clampf(edge_select.sharpAngleDeg - 1.0f, 1.0f, 89.0f);
                    edge_select.dirtyTopology = true;
                    edge_select.selectedEdge = -1;
                    edge_select.selectedChain = -1;
                } else if (face_select.enabled &&
                           (key == RGFW_equal || key == RGFW_kpPlus)) {
                    face_select.angleThresholdDeg = clampf(face_select.angleThresholdDeg + 1.0f, 1.0f, 85.0f);
                    face_select.dirty = true;
                    face_select.selectedRegion = -1;
                } else if (face_select.enabled &&
                           (key == RGFW_minus || key == RGFW_kpMinus)) {
                    face_select.angleThresholdDeg = clampf(face_select.angleThresholdDeg - 1.0f, 1.0f, 85.0f);
                    face_select.dirty = true;
                    face_select.selectedRegion = -1;
                } else if (key == RGFW_f) {
                    Vec3 obmin = {0.0f, 0.0f, 0.0f};
                    Vec3 obmax = {0.0f, 0.0f, 0.0f};
                    if (selected_scene_object_bounds(script_scene, selected_object_index, &obmin, &obmax)) {
                        focus_camera_on_bounds(obmin, obmax, fov_degrees, &distance, &target);
                    } else if (object_selected) {
                        focus_camera_on_bounds(mesh_bmin, mesh_bmax, fov_degrees, &distance, &target);
                    }
                } else if (key == RGFW_p) {
                    pick_debug_overlay = !pick_debug_overlay;
                } else if (key == RGFW_d) {
                    show_sketch_dimensions = !show_sketch_dimensions;
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
                    yaw_deg -= (float)dx * 0.35f;
                    pitch_deg += (float)dy * 0.35f;
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
                    if (g_ui.exportButtonData.found &&
                        point_in_rect(mouse_ui_x, mouse_ui_y,
                                      (int)g_ui.exportButtonData.boundingBox.x, (int)g_ui.exportButtonData.boundingBox.y,
                                      (int)g_ui.exportButtonData.boundingBox.width, (int)g_ui.exportButtonData.boundingBox.height)) {
                        const std::string out_name = vicad_runtime::MakeExport3mfFilename();
                        std::string export_error;
                        if (export_script_scene_3mf(&worker_client, script_path, out_name.c_str(), &export_error)) {
                            export_status_ok = true;
                            export_status_line = std::string("Saved: ") + out_name;
                            std::fprintf(stderr, "[vicad] exported 3mf: %s\n", out_name.c_str());
                        } else {
                            export_status_ok = false;
                            export_status_line = export_error.empty() ? "Export failed." : export_error;
                            std::fprintf(stderr, "[vicad] export error: %s\n", export_status_line.c_str());
                        }
                        continue;
                    }
                    if (g_ui.panelData.found &&
                        point_in_rect(mouse_ui_x, mouse_ui_y,
                                      (int)g_ui.panelData.boundingBox.x, (int)g_ui.panelData.boundingBox.y,
                                      (int)g_ui.panelData.boundingBox.width, (int)g_ui.panelData.boundingBox.height)) {
                        // Click on panel body should not affect 3D selection.
                        continue;
                    }
                    if (g_ui.infoPanelData.found &&
                        point_in_rect(mouse_ui_x, mouse_ui_y,
                                      (int)g_ui.infoPanelData.boundingBox.x, (int)g_ui.infoPanelData.boundingBox.y,
                                      (int)g_ui.infoPanelData.boundingBox.width, (int)g_ui.infoPanelData.boundingBox.height)) {
                        continue;
                    }
                    if (g_ui.exportPanelData.found &&
                        point_in_rect(mouse_ui_x, mouse_ui_y,
                                      (int)g_ui.exportPanelData.boundingBox.x, (int)g_ui.exportPanelData.boundingBox.y,
                                      (int)g_ui.exportPanelData.boundingBox.width, (int)g_ui.exportPanelData.boundingBox.height)) {
                        continue;
                    }

                    const Vec3 eye = camera_position(target, yaw_deg, pitch_deg, distance);
                    const CameraBasis basis = camera_basis(eye, target);
                    const Vec3 ray_dir =
                        camera_ray_direction(mouse_px_x, mouse_px_y, width, height, fov_degrees, basis);
                    if (edge_select.enabled) {
                        if (edge_select.dirtyTopology) {
                            edge_select.edges = vicad::BuildEdgeTopology(mesh, edge_select.sharpAngleDeg);
                            edge_select.dirtyTopology = false;
                            edge_select.dirtySilhouette = true;
                            if ((size_t)edge_select.selectedEdge >= edge_select.edges.edges.size()) {
                                edge_select.selectedEdge = -1;
                                edge_select.selectedChain = -1;
                            }
                        }
                        if (edge_select.dirtySilhouette) {
                            edge_select.silhouette = vicad::ComputeSilhouetteEdges(
                                mesh, edge_select.edges, (double)eye.x, (double)eye.y, (double)eye.z);
                            edge_select.dirtySilhouette = false;
                        }
                        const Vec3 diag = sub(mesh_bmax, mesh_bmin);
                        const double bboxDiag = std::sqrt((double)dot(diag, diag));
                        const double pickRadius = std::max(1e-4, bboxDiag * 0.012);
                        const int edge = vicad::PickEdgeByRay(
                            mesh, edge_select.edges, edge_select.silhouette,
                            (double)eye.x, (double)eye.y, (double)eye.z,
                            (double)ray_dir.x, (double)ray_dir.y, (double)ray_dir.z,
                            pickRadius,
                            nullptr);
                        edge_select.selectedEdge = edge;
                        edge_select.selectedChain = -1;
                        if (edge >= 0 &&
                            (size_t)edge < edge_select.edges.edgeFeatureChain.size()) {
                            edge_select.selectedChain = edge_select.edges.edgeFeatureChain[(size_t)edge];
                        }
                        object_selected = edge >= 0;
                    } else if (face_select.enabled) {
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
                        if (!script_scene.empty()) {
                            selected_object_index = pick_scene_object_by_ray(script_scene, eye, ray_dir);
                            object_selected = selected_object_index >= 0;
                        } else {
                            object_selected = ray_hits_mesh(mesh, eye, ray_dir);
                        }
                    }
                }
            }
        }

        if (height <= 0) height = 1;
        if (ui_height <= 0) ui_height = 1;
        const float display_scale = vicad_input::ComputeDisplayScale(width, height, window_w, window_h);
        const float hud_scale = kHudBaseScale * display_scale;
        ui_scale = 1.0f;
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

        if (edge_select.enabled) {
            if (edge_select.dirtyTopology) {
                edge_select.edges = vicad::BuildEdgeTopology(mesh, edge_select.sharpAngleDeg);
                edge_select.dirtyTopology = false;
                edge_select.dirtySilhouette = true;
                if ((size_t)edge_select.selectedEdge >= edge_select.edges.edges.size()) {
                    edge_select.selectedEdge = -1;
                    edge_select.selectedChain = -1;
                }
            }
            edge_select.silhouette = vicad::ComputeSilhouetteEdges(
                mesh, edge_select.edges, (double)eye.x, (double)eye.y, (double)eye.z);
            edge_select.dirtySilhouette = false;
            const Vec3 ray_dir =
                camera_ray_direction(mouse_px_x, mouse_px_y, width, height, fov_degrees, basis);
            const Vec3 diag = sub(mesh_bmax, mesh_bmin);
            const double bboxDiag = std::sqrt((double)dot(diag, diag));
            const double pickRadius = std::max(1e-4, bboxDiag * 0.012);
            edge_select.hoveredEdge = vicad::PickEdgeByRay(
                mesh, edge_select.edges, edge_select.silhouette,
                (double)eye.x, (double)eye.y, (double)eye.z,
                (double)ray_dir.x, (double)ray_dir.y, (double)ray_dir.z,
                pickRadius,
                nullptr);
            edge_select.hoveredChain = -1;
            if (edge_select.hoveredEdge >= 0 &&
                (size_t)edge_select.hoveredEdge < edge_select.edges.edgeFeatureChain.size()) {
                edge_select.hoveredChain = edge_select.edges.edgeFeatureChain[(size_t)edge_select.hoveredEdge];
            }
            face_select.hoveredRegion = -1;
            object_selected = edge_select.selectedEdge >= 0 || edge_select.hoveredEdge >= 0;
        } else if (face_select.enabled) {
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
            edge_select.hoveredEdge = -1;
            edge_select.hoveredChain = -1;
            hovered_object_index = -1;
        } else {
            edge_select.hoveredEdge = -1;
            edge_select.hoveredChain = -1;
            face_select.hoveredRegion = -1;
            if (!script_scene.empty()) {
                const Vec3 ray_dir =
                    camera_ray_direction(mouse_px_x, mouse_px_y, width, height, fov_degrees, basis);
                hovered_object_index = pick_scene_object_by_ray(script_scene, eye, ray_dir);
                object_selected = (selected_object_index >= 0 || hovered_object_index >= 0);
            } else {
                hovered_object_index = -1;
            }
        }

        const size_t scene_sketch_count = count_scene_sketches(script_scene);
        const vicad::ScriptSceneObject *info_object = nullptr;
        if (selected_object_index >= 0 &&
            (size_t)selected_object_index < script_scene.size()) {
            info_object = &script_scene[(size_t)selected_object_index];
        }
        Clay_RenderCommandArray ui_cmds = build_clay_ui(
            ui_width, ui_height, script_error.empty(), script_error,
            mesh.NumTri(), mesh.NumVert(), object_selected,
            script_scene.size(), scene_sketch_count,
            selected_object_index, hovered_object_index,
            selected_object_index >= 0 && (size_t)selected_object_index < script_scene.size()
                ? script_scene[(size_t)selected_object_index].objectId
                : 0ull,
            hovered_object_index >= 0 && (size_t)hovered_object_index < script_scene.size()
                ? script_scene[(size_t)hovered_object_index].objectId
                : 0ull,
            show_sketch_dimensions,
            info_object,
            hud_scale,
            face_select, edge_select,
            export_status_line, export_status_ok);

        draw_grid();
        draw_mesh(mesh);
        draw_script_sketches(script_scene, selected_object_index, hovered_object_index);
        if (feature_detection_enabled && edge_select.enabled) {
            draw_feature_edges(mesh, edge_select.edges);
            draw_silhouette_edges(mesh, edge_select.edges, edge_select.silhouette);
            if (edge_select.hoveredChain >= 0 &&
                edge_select.hoveredChain != edge_select.selectedChain) {
                draw_chain_overlay(mesh, edge_select.edges, edge_select.hoveredChain,
                                   4.0f, 0.38f, 0.73f, 1.0f, 0.96f);
            } else if (edge_select.hoveredEdge >= 0 &&
                       edge_select.hoveredEdge != edge_select.selectedEdge) {
                draw_hovered_edge(mesh, edge_select.edges, edge_select.hoveredEdge);
            }
            if (edge_select.selectedChain >= 0) {
                draw_chain_overlay(mesh, edge_select.edges, edge_select.selectedChain,
                                   5.2f, 1.0f, 0.90f, 0.16f, 1.0f);
            } else if (edge_select.selectedEdge >= 0) {
                draw_selected_edge(mesh, edge_select.edges, edge_select.selectedEdge);
            }
        }
        if (feature_detection_enabled && face_select.enabled) {
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
        if (object_selected && !face_select.enabled && !edge_select.enabled) {
            if (selected_object_index >= 0 &&
                (size_t)selected_object_index < script_scene.size()) {
                const vicad::ScriptSceneObject &obj = script_scene[(size_t)selected_object_index];
                if (scene_object_is_manifold(obj)) {
                    draw_mesh_selection_overlay(obj.mesh, 0.22f, 0.52f, 0.98f, 0.28f);
                }
            } else if (hovered_object_index >= 0 &&
                       (size_t)hovered_object_index < script_scene.size()) {
                const vicad::ScriptSceneObject &obj = script_scene[(size_t)hovered_object_index];
                if (scene_object_is_manifold(obj)) {
                    draw_mesh_selection_overlay(obj.mesh, 0.34f, 0.66f, 1.00f, 0.16f);
                }
            } else {
                draw_mesh_selection_overlay(mesh, 0.22f, 0.52f, 0.98f, 0.28f);
            }
        }
        DimensionRenderContext dim_ctx = {};
        dim_ctx.eye = eye;
        dim_ctx.camera = basis;
        dim_ctx.fovDegrees = fov_degrees;
        dim_ctx.viewportHeight = height;
        dim_ctx.arrowPixels = 4.0f;
        draw_script_sketch_dimensions(script_scene, selected_object_index, dim_ctx, show_sketch_dimensions);
        if (pick_debug_overlay) {
            draw_pick_debug_overlay(width, height, window_w, window_h,
                                    mouse_x, mouse_y, mouse_px_x, mouse_px_y);
        }
        const int status_bottom = g_ui.panelData.found
            ? (int)std::lround(g_ui.panelData.boundingBox.y + g_ui.panelData.boundingBox.height)
            : 0;
        const int info_bottom = g_ui.infoPanelData.found
            ? (int)std::lround(g_ui.infoPanelData.boundingBox.y + g_ui.infoPanelData.boundingBox.height)
            : 0;
        const int export_bottom = g_ui.exportPanelData.found
            ? (int)std::lround(g_ui.exportPanelData.boundingBox.y + g_ui.exportPanelData.boundingBox.height)
            : 0;
        int top_right_offset = status_bottom > info_bottom ? status_bottom : info_bottom;
        if (export_bottom > top_right_offset) top_right_offset = export_bottom;
        top_right_offset += 8;
        draw_orientation_cube(basis, width, height, top_right_offset, hud_scale);
        clay_render_commands(ui_cmds, width, height, ui_scale);

        RGFW_window_swapBuffers_OpenGL(win);
    }

    RGFW_window_close(win);
    return 0;
}
