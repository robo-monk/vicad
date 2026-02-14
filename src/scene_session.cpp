#include "scene_session.h"

#include <sys/stat.h>

#include <exception>
#include <utility>

#include "manifold/meshIO.h"

namespace vicad_scene {

namespace {

long long file_mtime_ns(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + (long long)st.st_mtimespec.tv_nsec;
#else
    return (long long)st.st_mtime * 1000000000LL;
#endif
}

bool compute_mesh_bounds(const manifold::MeshGL &mesh, vicad_app::Vec3 *bmin, vicad_app::Vec3 *bmax) {
    if (!bmin || !bmax || mesh.numProp < 3 || mesh.NumVert() == 0) return false;
    vicad_app::Vec3 mn = {
        mesh.vertProperties[0],
        mesh.vertProperties[1],
        mesh.vertProperties[2],
    };
    vicad_app::Vec3 mx = mn;
    for (size_t i = 1; i < mesh.NumVert(); ++i) {
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

bool scene_object_is_manifold(const vicad::ScriptSceneObject &obj) {
    return obj.kind == vicad::ScriptSceneObjectKind::Manifold;
}

const char *manifold_error_string(manifold::Manifold::Error error) {
    switch (error) {
        case manifold::Manifold::Error::NoError: return "No error";
        case manifold::Manifold::Error::NonFiniteVertex: return "Non-finite vertex";
        case manifold::Manifold::Error::NotManifold: return "Not manifold";
        case manifold::Manifold::Error::VertexOutOfBounds: return "Vertex out of bounds";
        case manifold::Manifold::Error::PropertiesWrongLength: return "Properties wrong length";
        case manifold::Manifold::Error::MissingPositionProperties: return "Missing position properties";
        case manifold::Manifold::Error::MergeVectorsDifferentLengths: return "Merge vectors different lengths";
        case manifold::Manifold::Error::MergeIndexOutOfBounds: return "Merge index out of bounds";
        case manifold::Manifold::Error::TransformWrongLength: return "Transform wrong length";
        case manifold::Manifold::Error::RunIndexWrongLength: return "Run index wrong length";
        case manifold::Manifold::Error::FaceIDWrongLength: return "Face id wrong length";
        case manifold::Manifold::Error::InvalidConstruction: return "Invalid construction";
        default: return "Unknown manifold error";
    }
}

bool export_mesh_to_3mf_native(const char *out_path, const manifold::MeshGL &mesh, std::string *error) {
    try {
        if (!out_path || out_path[0] == '\0') {
            if (error) *error = "Output path is empty.";
            return false;
        }
        if (mesh.NumVert() == 0 || mesh.NumTri() == 0) {
            if (error) *error = "Mesh is empty; nothing to export.";
            return false;
        }
        manifold::ExportMesh(out_path, mesh, manifold::ExportOptions{});
        return true;
    } catch (const std::exception &e) {
        if (error) *error = std::string("3MF export failed: ") + e.what();
        return false;
    } catch (...) {
        if (error) *error = "3MF export failed with unknown exception.";
        return false;
    }
}

}  // namespace

bool SceneSessionComputeSceneBounds(const std::vector<vicad::ScriptSceneObject> &scene,
                                    vicad_app::Vec3 *bmin,
                                    vicad_app::Vec3 *bmax) {
    if (!bmin || !bmax || scene.empty()) return false;
    bool initialized = false;
    for (const vicad::ScriptSceneObject &obj : scene) {
        const vicad_app::Vec3 mn = {obj.bmin.x, obj.bmin.y, obj.bmin.z};
        const vicad_app::Vec3 mx = {obj.bmax.x, obj.bmax.y, obj.bmax.z};
        if (!initialized) {
            *bmin = mn;
            *bmax = mx;
            initialized = true;
            continue;
        }
        if (mn.x < bmin->x) bmin->x = mn.x;
        if (mn.y < bmin->y) bmin->y = mn.y;
        if (mn.z < bmin->z) bmin->z = mn.z;
        if (mx.x > bmax->x) bmax->x = mx.x;
        if (mx.y > bmax->y) bmax->y = mx.y;
        if (mx.z > bmax->z) bmax->z = mx.z;
    }
    return initialized;
}

bool SceneSessionReloadIfChanged(SceneSessionState *state,
                                 vicad::ScriptWorkerClient *worker_client,
                                 const vicad::ReplayLodPolicy &lod_policy,
                                 std::string *err) {
    if (err) err->clear();
    if (!state || !worker_client) {
        if (err) *err = "SceneSessionReloadIfChanged received invalid inputs.";
        return false;
    }

    const long long mt = file_mtime_ns(state->script_path.c_str());
    if (mt < 0 || mt == state->last_mtime_ns) return true;
    state->last_mtime_ns = mt;

    std::string local_err;
    std::vector<vicad::ScriptSceneObject> next_scene;
    bool loaded = worker_client->ExecuteScriptScene(state->script_path.c_str(), &next_scene, &local_err, lod_policy);

    manifold::MeshGL next_mesh;
    vicad_app::Vec3 next_bmin = {0.0f, 0.0f, 0.0f};
    vicad_app::Vec3 next_bmax = {0.0f, 0.0f, 0.0f};

    if (loaded) {
        std::vector<manifold::Manifold> parts;
        parts.reserve(next_scene.size());
        for (const vicad::ScriptSceneObject &obj : next_scene) {
            if (scene_object_is_manifold(obj)) parts.push_back(obj.manifold);
        }

        if (!parts.empty()) {
            manifold::Manifold merged = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
            if (merged.Status() != manifold::Manifold::Error::NoError) {
                loaded = false;
                local_err = std::string("Scene merge failed: ") + manifold_error_string(merged.Status());
            } else {
                next_mesh = merged.GetMeshGL();
                if (!compute_mesh_bounds(next_mesh, &next_bmin, &next_bmax)) {
                    loaded = false;
                    local_err = "Merged scene mesh has no valid bounds.";
                }
            }
        } else {
            next_mesh.numProp = 3;
            if (!SceneSessionComputeSceneBounds(next_scene, &next_bmin, &next_bmax)) {
                loaded = false;
                local_err = "Scene has no manifold or sketch geometry to visualize.";
            }
        }
    }

    if (!loaded && !worker_client->started()) {
        state->ipc_start_failed = true;
    }

    if (loaded) {
        state->scene_objects = std::move(next_scene);
        state->merged_mesh = std::move(next_mesh);

        bool have_mesh_bounds = compute_mesh_bounds(state->merged_mesh, &state->bounds_min, &state->bounds_max);
        vicad_app::Vec3 scene_bmin = {0.0f, 0.0f, 0.0f};
        vicad_app::Vec3 scene_bmax = {0.0f, 0.0f, 0.0f};
        const bool have_scene_bounds = SceneSessionComputeSceneBounds(state->scene_objects, &scene_bmin, &scene_bmax);
        if (!have_mesh_bounds && have_scene_bounds) {
            state->bounds_min = scene_bmin;
            state->bounds_max = scene_bmax;
            have_mesh_bounds = true;
        } else if (have_mesh_bounds && have_scene_bounds) {
            if (scene_bmin.x < state->bounds_min.x) state->bounds_min.x = scene_bmin.x;
            if (scene_bmin.y < state->bounds_min.y) state->bounds_min.y = scene_bmin.y;
            if (scene_bmin.z < state->bounds_min.z) state->bounds_min.z = scene_bmin.z;
            if (scene_bmax.x > state->bounds_max.x) state->bounds_max.x = scene_bmax.x;
            if (scene_bmax.y > state->bounds_max.y) state->bounds_max.y = scene_bmax.y;
            if (scene_bmax.z > state->bounds_max.z) state->bounds_max.z = scene_bmax.z;
        } else if (!have_mesh_bounds) {
            state->bounds_min = next_bmin;
            state->bounds_max = next_bmax;
        }
        state->error_text.clear();
        return true;
    }

    if (state->ipc_start_failed && local_err.empty()) local_err = "IPC startup failed.";
    state->error_text = local_err;
    if (err) *err = local_err;
    return false;
}

bool SceneSessionExport3mf(SceneSessionState *state,
                           vicad::ScriptWorkerClient *worker_client,
                           std::string *out_path,
                           std::string *err) {
    if (err) err->clear();
    if (!state || !worker_client || !out_path || out_path->empty()) {
        if (err) *err = "SceneSessionExport3mf received invalid inputs.";
        return false;
    }

    std::string local_err;
    std::vector<vicad::ScriptSceneObject> scene_objects;
    vicad::ReplayLodPolicy lod_policy = {};
    lod_policy.profile = vicad::LodProfile::Export3MF;
    if (!worker_client->ExecuteScriptScene(state->script_path.c_str(), &scene_objects, &local_err, lod_policy)) {
        if (err) *err = local_err;
        return false;
    }

    std::vector<manifold::Manifold> parts;
    parts.reserve(scene_objects.size());
    for (const vicad::ScriptSceneObject &obj : scene_objects) {
        if (obj.kind == vicad::ScriptSceneObjectKind::Manifold) parts.push_back(obj.manifold);
    }
    if (parts.empty()) {
        if (err) *err = "Script scene does not contain manifold geometry to export.";
        return false;
    }

    manifold::Manifold merged = manifold::Manifold::BatchBoolean(parts, manifold::OpType::Add);
    if (merged.Status() != manifold::Manifold::Error::NoError) {
        if (err) *err = "Failed to merge scene objects for mesh export.";
        return false;
    }

    manifold::MeshGL export_mesh = merged.GetMeshGL();
    return export_mesh_to_3mf_native(out_path->c_str(), export_mesh, err);
}

}  // namespace vicad_scene
