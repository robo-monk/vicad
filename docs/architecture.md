# Architecture

## Overview

VICAD is a two-process application:

1. **C++ main process** — owns the window, OpenGL rendering, user input, and scene state.
2. **Bun worker process** — executes user `.vicad.ts` scripts and encodes the resulting
   geometry operations into a shared memory buffer.

Communication uses a Unix domain socket for signalling and `mmap`-backed shared memory
for bulk data transfer. See [`ipc-protocol.md`](ipc-protocol.md) for the wire format.

## Module Map

```
src/
  ipc_protocol.h          ← Shared types (OpCode, IpcState, wire structs). No deps.
  op_decoder.cpp/h        ← Deserialises op stream from shared memory.
  op_reader.cpp/h         ← Low-level binary reader helpers.
  scene_session.cpp/h     ← Owns scene objects, file-watch, mesh bounds.
  scene_runtime.cpp/h     ← Manages script worker lifecycle.
  script_worker_client.cpp/h  ← Unix socket + shm IPC with Bun worker.
  picking.cpp/h           ← Ray-cast face/edge selection.
  edge_detection.cpp/h    ← Derives selectable edges from mesh topology.
  face_detection.cpp/h    ← Derives selectable faces from mesh topology.
  render_scene.cpp/h      ← 3D geometry draw calls.
  render_ui.cpp/h         ← Clay UI draw calls.
  renderer_3d.cpp/h       ← OpenGL backend for 3D rendering.
  renderer_overlay.cpp/h  ← OpenGL overlay (HUD, annotations).
  ui_layout.cpp/h         ← Clay layout definitions.
  ui_state.cpp/h          ← UI state structs.
  input_controller.cpp/h  ← RGFW input → internal events.
  event_router.cpp/h      ← Routes input events to handlers.
  interaction_state.cpp/h ← Active tool / selection state.
  lod_policy.cpp/h        ← Level-of-detail mesh simplification policy.
  sketch_semantics.cpp    ← Analyses CrossSection geometry for UI hints.
  sketch_dimensions.cpp/h ← Dimension annotation overlay data.
  app_state.h             ← Shared value types (Vec2, Vec3, CameraBasis, …).
  app_kernel.cpp/h        ← Main loop, event dispatch, frame orchestration.
  main.cpp                ← Entry point.

worker/
  worker.ts               ← Entry point; manages shm, socket, script execution.
  ipc_protocol.ts         ← Mirrors src/ipc_protocol.h (keep in sync manually).
  op-encoder.ts           ← Encodes Manifold/CrossSection ops to binary format.
  proxy-manifold.ts       ← Wraps manifold-3d npm package; intercepts calls to encode ops.
  proxy-manifold.test.ts  ← Unit tests for proxy-manifold.
```

## Dependency Directions

Arrows mean "may import". Violations are bugs.

```
ipc_protocol.h
  ↓
op_reader → op_decoder
  ↓
script_worker_client ← scene_runtime ← scene_session
  ↓                         ↓
picking               sketch_semantics
edge_detection
face_detection
  ↓
render_scene → renderer_3d
render_ui    → renderer_overlay
  ↓
app_kernel (coordinates all of the above)
```

`app_state.h` is a pure value-type header; anything may include it.
`ipc_protocol.h` is a pure protocol header; anything may include it.
`app_kernel` is the only file allowed to include everything.

**Enforced by `tools/check-layers.sh` (runs as step 0 of `./nob test`):**
- `ipc_protocol.h`, `edge_detection.h`, `face_detection.h`, `lod_policy.h` — no local src includes
- `op_reader`, `op_decoder` — must not include scene or render modules
- `renderer_3d`, `renderer_overlay`, `render_ui`, `ui_layout`, `ui_state` — must not include scene or IPC client headers

**Documented exemptions (intentional, not flagged by the check):**
- `render_scene.h` includes `script_worker_client.h` — needs `ScriptSceneObject` types to draw
- `picking.h` includes `script_worker_client.h` — needs `ScriptSceneObject` types for ray-cast
- `event_router.h` includes `scene_session.h` — routes input events into the active scene

## Worker Dependency Directions

```
ipc_protocol.ts
  ↓
op-encoder.ts
  ↓
proxy-manifold.ts
  ↓
worker.ts
```

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Unix socket + `mmap` shm | Avoids serialisation overhead for large mesh buffers |
| Bun worker (TypeScript) | Users write scripts in TS; Bun has fast FFI for shm ops |
| Nob build system | Single-file C build script; no CMake dependency for the app itself |
| Clay immediate-mode UI | Single-header, no retained state, easy to reason about |
| Manifold 3D | Robust boolean CSG; well-documented C++ and npm APIs |
| RGFW | Single-header cross-platform windowing without GLFW baggage |
