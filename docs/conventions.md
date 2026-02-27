# Coding Conventions

## General

- Write lean, simple, and readable code. Prefer clarity over cleverness.
- No fallbacks, no backwards-compatibility shims, no feature flags.
- No dead code. Delete it.

## C++ (src/)

### Naming

| Thing | Convention | Example |
|-------|-----------|---------|
| Files | `snake_case` | `edge_detection.cpp` |
| Types / structs | `PascalCase` | `SceneObject` |
| Functions | `snake_case` | `compute_mesh_bounds` |
| Local variables | `snake_case` | `mesh_count` |
| Constants | `kPascalCase` | `kIpcVersion` |
| Namespaces | `snake_case` | `vicad_scene` |
| Enum values | `PascalCase` | `IpcState::RequestReady` |

### Structure

- One logical unit per file pair (`.cpp` / `.h`). No catch-all files.
- Maximum file length: 600 lines. If a file exceeds this, split it.
- `app_kernel.cpp` is the only permitted exception (coordinator).
- Headers use `#ifndef` include guards, not `#pragma once`.
- No raw `printf` / `fprintf` in application code. Use the structured log
  macro once introduced (see `docs/exec-plans/index.md`).

### Error handling

- Functions that can fail return `bool` and take an `std::string *error` out-param.
- No exceptions in application code (vendored libs may throw; catch at boundaries).
- No silent failures — every error path must set the error string or log.

### Includes

- System headers before local headers.
- Local headers use `"quotes"`, not `<angles>`.
- Vendored single-header libs (`RGFW.h`, `clay.h`) are included exactly once,
  with their `_IMPLEMENTATION` define, in `app_kernel.cpp` only.

## TypeScript (worker/)

### Naming

| Thing | Convention | Example |
|-------|-----------|---------|
| Files | `kebab-case` | `proxy-manifold.ts` |
| Types / interfaces | `PascalCase` | `OpRecord` |
| Functions / variables | `camelCase` | `encodeOp` |
| Constants | `UPPER_SNAKE` | `IPC_VERSION` |

### Structure

- No `console.log` in worker code. Use the structured logger (to be added).
- No `any` types.
- Tests live alongside source as `*.test.ts` and run with `bun test`.

## IPC Protocol Changes

Whenever `src/ipc_protocol.h` is modified:
1. Update `worker/ipc_protocol.ts` to match.
2. Increment `kIpcVersion` / `IPC_VERSION` in both files.
3. Update `docs/ipc-protocol.md`.
4. Run `./nob build --asan && ./nob test` before committing.

## Vendored Dependencies

Do not edit anything under `manifold/`, `Clipper2/`, `assimp/`, `freetype/`,
`harfbuzz/`, or `nativefiledialog/`. Patches go upstream or are applied via
the build system.
