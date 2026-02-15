#ifndef VICAD_SCENE_SESSION_H_
#define VICAD_SCENE_SESSION_H_

#include <string>
#include <vector>

#include "app_state.h"
#include "lod_policy.h"
#include "script_worker_client.h"

namespace vicad_scene {

struct SceneSessionState {
    std::string script_path;
    long long last_mtime_ns = -1;
    long long last_ctime_ns = -1;
    long long last_size_bytes = -1;
    std::string error_text;
    manifold::MeshGL merged_mesh;
    std::vector<vicad::ScriptSceneObject> scene_objects;
    vicad_app::Vec3 bounds_min = {0.0f, 0.0f, 0.0f};
    vicad_app::Vec3 bounds_max = {0.0f, 0.0f, 0.0f};
    bool ipc_start_failed = false;
};

bool SceneSessionComputeSceneBounds(const std::vector<vicad::ScriptSceneObject> &scene,
                                    vicad_app::Vec3 *bmin,
                                    vicad_app::Vec3 *bmax);

bool SceneSessionReloadIfChanged(SceneSessionState *state,
                                 vicad::ScriptWorkerClient *worker_client,
                                 const vicad::ReplayLodPolicy &lod_policy,
                                 std::string *err);

bool SceneSessionExport3mf(SceneSessionState *state,
                           vicad::ScriptWorkerClient *worker_client,
                           std::string *out_path,
                           std::string *err);

}  // namespace vicad_scene

#endif  // VICAD_SCENE_SESSION_H_
