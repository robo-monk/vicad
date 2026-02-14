#include "ui_layout.h"

namespace vicad_ui {

UiHitTarget HitTestUi(int x, int y,
                      int panel_x, int panel_y, int panel_w, int panel_h,
                      bool panel_found) {
    if (!panel_found) return UiHitTarget::None;
    if (x >= panel_x && y >= panel_y && x < panel_x + panel_w && y < panel_y + panel_h) {
        return UiHitTarget::StatusPanel;
    }
    return UiHitTarget::None;
}

}  // namespace vicad_ui
