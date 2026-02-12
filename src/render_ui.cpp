#include "render_ui.h"

#include "app_state.h"

namespace vicad_render_ui {

float ClampHudScale(float hud_scale) {
  return vicad_app::clampf(hud_scale, 0.75f, 3.0f);
}

}  // namespace vicad_render_ui
