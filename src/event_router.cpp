#include "event_router.h"

namespace vicad_event {

bool HandleWindowEvent(vicad_interaction::InteractionState *interaction,
                       vicad_scene::SceneSessionState *scene,
                       RGFW_window *window,
                       const RGFW_event &event,
                       std::string *err) {
    (void)scene;
    (void)window;
    if (!interaction) {
        if (err) *err = "HandleWindowEvent received null interaction state.";
        return false;
    }
    if (event.type == RGFW_keyPressed && event.key.value == RGFW_d) {
        interaction->show_sketch_dimensions = !interaction->show_sketch_dimensions;
    }
    if (event.type == RGFW_keyPressed && event.key.value == RGFW_p) {
        interaction->pick_debug_overlay = !interaction->pick_debug_overlay;
    }
    return true;
}

}  // namespace vicad_event
