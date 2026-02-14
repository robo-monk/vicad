#ifndef VICAD_UI_STATE_H_
#define VICAD_UI_STATE_H_

#include <string>

namespace vicad_ui {

struct UiState {
    bool panel_collapsed = false;
    std::string export_status_line = "Ready (uses Export3MF LOD profile)";
    bool export_status_ok = true;
};

struct UiInput {
    int width = 0;
    int height = 0;
};

struct UiFrameResult {
    int reserved = 0;
};

UiFrameResult BuildUiFrame(UiState *state, const UiInput &input);

}  // namespace vicad_ui

#endif  // VICAD_UI_STATE_H_
