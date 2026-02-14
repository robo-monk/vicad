#ifndef VICAD_EVENT_ROUTER_H_
#define VICAD_EVENT_ROUTER_H_

#include <string>

#include "../RGFW.h"
#include "interaction_state.h"
#include "scene_session.h"

namespace vicad_event {

bool HandleWindowEvent(vicad_interaction::InteractionState *interaction,
                       vicad_scene::SceneSessionState *scene,
                       RGFW_window *window,
                       const RGFW_event &event,
                       std::string *err);

}  // namespace vicad_event

#endif  // VICAD_EVENT_ROUTER_H_
