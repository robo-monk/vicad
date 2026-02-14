#ifndef VICAD_PICKING_H_
#define VICAD_PICKING_H_

#include <vector>

#include "app_state.h"
#include "script_worker_client.h"

namespace vicad_picking {

struct PickContext {
    int viewport_width = 1;
    int viewport_height = 1;
    float fov_degrees = 65.0f;
    vicad_app::Vec3 eye = {0.0f, 0.0f, 0.0f};
    vicad_app::CameraBasis basis = {};
};

vicad_app::Vec3 CameraRayDirection(int mouse_x, int mouse_y, const PickContext &ctx);

void WindowMouseToPixel(int mouse_x, int mouse_y,
                        int window_w, int window_h,
                        int pixel_w, int pixel_h,
                        int *out_px, int *out_py);

bool PickMeshHit(const manifold::MeshGL &mesh,
                 const vicad_app::Vec3 &eye,
                 const vicad_app::Vec3 &ray_dir);

int PickSceneObject(const std::vector<vicad::ScriptSceneObject> &scene,
                    const vicad_app::Vec3 &eye,
                    const vicad_app::Vec3 &ray_dir);

}  // namespace vicad_picking

#endif  // VICAD_PICKING_H_
