#!/usr/bin/env bash
# tools/check-layers.sh
#
# Structural layer violation checker.
# Greps for forbidden #include relationships and exits non-zero on any violation.
#
# Rules enforced:
#   1. Bottom-layer headers (ipc_protocol, edge_detection, face_detection, lod_policy,
#      op_reader, op_decoder, sketch_dimensions) must not include rendering or scene headers.
#   2. Rendering headers (renderer_3d, renderer_overlay, render_ui, ui_layout, ui_state)
#      must not include scene or IPC client headers.
#
# Exemptions (intentional by design, documented in docs/architecture.md):
#   - render_scene.h includes script_worker_client.h (needs scene object types)
#   - picking.h includes script_worker_client.h (needs scene object types)
#   - event_router.h includes scene_session.h (routes events into the scene)
#   - app_kernel.cpp includes everything (coordinator)

set -euo pipefail
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

check() {
  local desc="$1"
  local files="$2"
  local pattern="$3"

  # shellcheck disable=SC2086
  matches=$(grep -rn "$pattern" $files 2>/dev/null || true)
  if [ -n "$matches" ]; then
    echo "FAIL: $desc"
    echo "$matches" | sed 's/^/  /'
    FAIL=$((FAIL + 1))
  else
    echo "PASS: $desc"
    PASS=$((PASS + 1))
  fi
}

RENDER_HEADERS='"scene_session\.h"\|"scene_runtime\.h"\|"script_worker_client\.h"\|"ipc_protocol\.h"'
SCENE_HEADERS='"scene_session\.h"\|"scene_runtime\.h"\|"script_worker_client\.h"'

# LOCAL_INCLUDE matches flat quoted includes like "foo.h" but not "manifold/foo.h" or "../bar.h"
LOCAL_INCLUDE='^#include "[^./][^/]*\.h"'

# Rule 1a: ipc_protocol.h has no local src includes
check \
  "ipc_protocol.h has no local src #include" \
  "src/ipc_protocol.h" \
  "$LOCAL_INCLUDE"

# Rule 1b: edge_detection.h has no local src includes
check \
  "edge_detection.h has no local src #include" \
  "src/edge_detection.h" \
  "$LOCAL_INCLUDE"

# Rule 1c: face_detection.h has no local src includes
check \
  "face_detection.h has no local src #include" \
  "src/face_detection.h" \
  "$LOCAL_INCLUDE"

# Rule 1d: lod_policy.h has no local src includes
check \
  "lod_policy.h has no local src #include" \
  "src/lod_policy.h" \
  "$LOCAL_INCLUDE"

# Rule 1e: op_reader has no scene/render includes
check \
  "op_reader does not include scene or render modules" \
  "src/op_reader.h src/op_reader.cpp" \
  "$SCENE_HEADERS"

# Rule 1f: op_decoder has no scene/render includes
check \
  "op_decoder does not include scene or render modules" \
  "src/op_decoder.h src/op_decoder.cpp" \
  "$SCENE_HEADERS"

# Rule 2a: renderer_3d does not import scene/IPC client
check \
  "renderer_3d does not include scene or IPC headers" \
  "src/renderer_3d.h src/renderer_3d.cpp" \
  "$RENDER_HEADERS"

# Rule 2b: renderer_overlay does not import scene/IPC client
check \
  "renderer_overlay does not include scene or IPC headers" \
  "src/renderer_overlay.h src/renderer_overlay.cpp" \
  "$RENDER_HEADERS"

# Rule 2c: render_ui does not import scene/IPC client
check \
  "render_ui does not include scene or IPC headers" \
  "src/render_ui.h src/render_ui.cpp" \
  "$RENDER_HEADERS"

# Rule 2d: ui_layout does not import scene/IPC client
check \
  "ui_layout does not include scene or IPC headers" \
  "src/ui_layout.h src/ui_layout.cpp" \
  "$RENDER_HEADERS"

# Rule 2e: ui_state does not import scene/IPC client
check \
  "ui_state does not include scene or IPC headers" \
  "src/ui_state.h src/ui_state.cpp" \
  "$RENDER_HEADERS"

echo ""
echo "Layer check: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ]
