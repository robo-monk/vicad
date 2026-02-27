# Plan: Agent-First Development Environment

## Goal

Make VICAD's repository the single source of truth for agents: readable,
navigable, linted, and observable without human translation.

## Status: In Progress

## Tasks

### P0 — Repository as Source of Truth ✅
- [x] Expand `AGENTS.md` to a functional table of contents
- [x] Create `docs/architecture.md` (module map, dependency directions, decisions)
- [x] Create `docs/ipc-protocol.md` (wire format reference)
- [x] Create `docs/conventions.md` (naming, structure, error handling)
- [x] Create `docs/exec-plans/` skeleton

### P1 — Structured Logging & Socket Parameterisation ✅
- [x] Add `src/log.h` — `vicad::Log(event, run_id, details)` emitting JSON lines to stderr
- [x] Wire `ScriptWorkerClient::LogEvent` through `vicad::Log`
- [x] Add `log()` helper to `worker/worker.ts` emitting JSON lines to stderr
- [x] Socket path already PID-namespaced (`/tmp/vicad-worker-<pid>.sock`) — no env var needed

### P1 — TypeScript Lint ✅
- [x] Add ESLint config (`eslint.config.js`) for `worker/` — no `console`, no `any`
- [x] Add `./nob lint-ts` target (shells to `bun run lint-ts`)
- [x] Fix pre-existing `any` violations in `worker.ts` globalThis injections

### P2 — C++ Lint ✅
- [x] Add `src/.clang-tidy` config (bugprone-*, cppcoreguidelines-pro-type-cstyle-cast, readability-identifier-naming)
- [x] Add `./nob lint-cpp` target (runs clang-tidy on core src/*.cpp files)

### P2 — Layer Violation Test ✅
- [x] `tools/check-layers.sh` — greps for 11 forbidden cross-layer #include rules
- [x] Hooked into `./nob test` as step 0 (now 4 steps total)

### P2 — Doc Link Checker ✅
- [x] `tools/check-docs.sh` — extracts markdown links from AGENTS.md and docs/, verifies paths resolve
- [x] `./nob lint-docs` target

### Logging Completion ✅
- [x] Convert 5 raw `fprintf` calls in `app_kernel.cpp` and `main.cpp` to `vicad::log_event`
- [x] Fix `vicad::Log` → `vicad::log_event` (snake_case per conventions)
- [x] Add JSON escaping to `log.h` so multiline/quoted strings are safe
- [x] All stderr output from vicad is now newline-delimited JSON

### P3 — End-to-End IPC Test ✅
- [x] `src/ipc_integration_test.cpp` — spawns bun worker, runs `sketch-fillet-example.vicad.ts`,
      verifies 2 scene objects with correct names, kinds, contours, mesh bounds (11 assertions)
- [x] `build_ipc_integration_test` in nob.c links ScriptWorkerClient + op stack
- [x] Hooked into `./nob test` as step 4/5

### P3 — Closed-Loop Agent Target ✅
- [x] `src/run_script.cpp` — parameterised script runner; stdout: JSON `{result, script, objects|error}`
- [x] `build/run_script` built by default `./nob` (not only during test)
- [x] `./nob agent-check` — build → layers → lint-ts → lint-docs → ipc; stdout: JSON verdict
- [x] `./nob agent-check --script=<path>` — same but step 5 runs the given script
- [x] Verbose check output redirected to stderr; stdout carries only JSON line(s)
- [x] Exit 0 = all checks pass, exit 1 = any failure
