#ifndef VICAD_UI_LAYOUT_H_
#define VICAD_UI_LAYOUT_H_

namespace vicad_ui {

enum class UiHitTarget {
    None,
    StatusPanel,
    InfoPanel,
    ExportPanel,
    ExportButton,
    Header,
};

UiHitTarget HitTestUi(int x, int y,
                      int panel_x, int panel_y, int panel_w, int panel_h,
                      bool panel_found);

}  // namespace vicad_ui

#endif  // VICAD_UI_LAYOUT_H_
