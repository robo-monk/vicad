#include "input_controller.h"

#include "app_state.h"

namespace vicad_input {

float ComputeDisplayScale(int pixel_w, int pixel_h, int window_w, int window_h) {
  float sx = 1.0f;
  float sy = 1.0f;
  if (window_w > 0 && pixel_w > 0) sx = (float)pixel_w / (float)window_w;
  if (window_h > 0 && pixel_h > 0) sy = (float)pixel_h / (float)window_h;
  return vicad_app::clampf((sx + sy) * 0.5f, 1.0f, 4.0f);
}

}  // namespace vicad_input
