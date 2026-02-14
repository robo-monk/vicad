#ifndef VICAD_INTERACTION_STATE_H_
#define VICAD_INTERACTION_STATE_H_

#include "app_state.h"

namespace vicad_interaction {

enum class SelectionMode {
    Object,
    Edge,
    Face,
};

struct InteractionState {
    vicad_app::Vec3 target = {0.0f, 0.0f, 0.0f};
    float yaw_deg = 45.0f;
    float pitch_deg = 24.0f;
    float distance = 80.0f;
    bool object_selected = false;
    int selected_object_index = -1;
    int hovered_object_index = -1;
    bool show_sketch_dimensions = true;
    bool pick_debug_overlay = true;
    SelectionMode selection_mode = SelectionMode::Object;
};

}  // namespace vicad_interaction

#endif  // VICAD_INTERACTION_STATE_H_
