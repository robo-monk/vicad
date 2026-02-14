#ifndef VICAD_RENDERER_OVERLAY_H_
#define VICAD_RENDERER_OVERLAY_H_

namespace vicad_renderer_overlay {

struct RenderOverlayInputs {
    int pixel_width = 0;
    int pixel_height = 0;
};

void RenderOverlays(const RenderOverlayInputs &in);

}  // namespace vicad_renderer_overlay

#endif  // VICAD_RENDERER_OVERLAY_H_
