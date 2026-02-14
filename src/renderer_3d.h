#ifndef VICAD_RENDERER_3D_H_
#define VICAD_RENDERER_3D_H_

#include "manifold/manifold.h"

namespace vicad_renderer3d {

struct RenderSceneInputs {
    const manifold::MeshGL *mesh = nullptr;
};

void RenderScene3D(const RenderSceneInputs &in);

}  // namespace vicad_renderer3d

#endif  // VICAD_RENDERER_3D_H_
