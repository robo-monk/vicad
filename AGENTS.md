Write lean, simple, and readable code.

This is a greenfield app — no users, no fallbacks, no backwards-compatibility shims.
Code quality must be production-grade.

## What is VICAD?

A native CAD application for 3D solid modeling. Users write TypeScript scripts
(`.vicad.ts`) that execute in an isolated Bun worker process. The worker encodes
geometry operations over Unix socket + shared memory IPC; the C++ main process
decodes and renders them via OpenGL.

## Agent Workflow

After making any change, verify it with a single command:

```bash
./nob agent-check 2>build/agent.log
```

Stdout emits one JSON verdict line:

```json
{"result":"pass","checks":{"build":true,"layers":true,"lint-ts":true,"lint-cpp":true,"lint-docs":true,"ipc":true}}
```

Exit 0 = all checks pass. Exit 1 = at least one failed.

To also run a specific script through the full IPC stack:

```bash
./nob agent-check --script=sketch-fillet-example.vicad.ts 2>build/agent.log
```

This adds a second stdout line with per-script detail:

```json
{"result":"pass","script":"sketch-fillet-example.vicad.ts","objects":2}
{"result":"pass","checks":{"build":true,"layers":true,"lint-ts":true,"lint-cpp":true,"lint-docs":true,"script":true}}
```

To diagnose a failure, query the structured log:

```bash
grep '"event"' build/agent.log | jq .
grep '"result":"fail"' build/agent.log | jq .
```

All vicad and worker output is newline-delimited JSON (`src`, `event`, `run_id`, optional `details`).

## Individual Checks

| Command | What it checks |
|---------|----------------|
| `./nob agent-check` | Build + all checks below + IPC integration test |
| `./nob lint-ts` | ESLint on `worker/` (no `console`, no `any`) |
| `./nob lint-cpp` | clang-tidy on `src/` (requires prior build) |
| `./nob lint-docs` | Markdown links in `AGENTS.md`/`docs/` resolve + op-code sync across C++/TS/docs |
| `./nob test` | Full suite: layer check, lod_replay_test, bun tests, IPC test, smoke test |
| `build/run_script <path>` | Run one script end-to-end; stdout: JSON `{result, objects\|error}` |

## Build

```bash
./nob            # compile everything including build/run_script
./nob --asan     # compile with AddressSanitizer
./nob test       # compile + run full test suite
```

## Docs

| Topic | File |
|-------|------|
| Architecture & module map | [`docs/architecture.md`](docs/architecture.md) |
| IPC protocol reference | [`docs/ipc-protocol.md`](docs/ipc-protocol.md) |
| Coding conventions | [`docs/conventions.md`](docs/conventions.md) |
| In-flight plans | [`docs/exec-plans/index.md`](docs/exec-plans/index.md) |
| Clay UI reference | [`docs/references/clay.md`](docs/references/clay.md) |
| Manifold CAD reference | [`docs/references/manifold-cad.md`](docs/references/manifold-cad.md) |

## Key Directories

| Path | Contents |
|------|----------|
| `src/` | C++ application (app kernel, rendering, scene, IPC client) |
| `worker/` | Bun TypeScript worker (script execution, op encoding) |
| `docs/` | Architecture, conventions, plans |
| `manifold/`, `Clipper2/`, etc. | Vendored C++ dependencies (do not edit) |
