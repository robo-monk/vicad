#include "render_scene.h"

namespace vicad_render_scene {

const char *SceneObjectKindName(vicad::ScriptSceneObjectKind kind) {
  switch (kind) {
    case vicad::ScriptSceneObjectKind::Manifold: return "MANIFOLD";
    case vicad::ScriptSceneObjectKind::CrossSection: return "SKETCH";
    default: return "UNKNOWN";
  }
}

}  // namespace vicad_render_scene
