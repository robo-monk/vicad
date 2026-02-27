# IPC Protocol

## Overview

The C++ main process and Bun worker communicate over:

1. **Unix domain socket** — control plane (connect, signal ready, disconnect).
2. **`mmap` shared memory** — data plane (request payload, response op stream).

The shared memory region is created by the main process and its file path is
passed to the worker at startup. Both sides map the same file.

## Versioning

```
kIpcVersion = 3   (src/ipc_protocol.h, worker/ipc_protocol.ts)
```

Both files must be updated together whenever the protocol changes.

## Shared Memory Layout

```
Offset 0         SharedHeader (60 bytes, packed)
Offset 4096      RequestPayload + script path string
Offset 1,048,576 ResponsePayload (Ok, Error, or Scene) + op records
```

Offsets are constants in `ipc_protocol.h`:

| Constant | Value |
|----------|-------|
| `kDefaultRequestOffset` | 4096 |
| `kDefaultResponseOffset` | 1,048,576 |
| `kDefaultShmSize` | 100 MiB |

## SharedHeader (60 bytes)

| Field | Type | Description |
|-------|------|-------------|
| `magic` | `char[8]` | `"VCADIPC1"` |
| `version` | `uint32_t` | Must equal `kIpcVersion` |
| `capacity_bytes` | `uint32_t` | Total shm size |
| `request_seq` | `uint64_t` | Monotonic request counter |
| `response_seq` | `uint64_t` | Echoed by worker on completion |
| `request_offset` | `uint32_t` | Byte offset of request payload |
| `request_length` | `uint32_t` | Byte length of request payload |
| `response_offset` | `uint32_t` | Byte offset of response payload |
| `response_length` | `uint32_t` | Byte length of response payload |
| `state` | `uint32_t` | `IpcState` enum |
| `error_code` | `uint32_t` | `IpcErrorCode` enum |
| `reserved` | `uint32_t` | Padding |

`static_assert(sizeof(SharedHeader) == 60)` enforces this.

## State Machine

```
Idle → RequestReady   (main sets script path + seq, sets state)
     → RequestRunning (worker picks up request)
     → ResponseReady  (worker writes ops, echoes seq)
     → Idle           (main reads response, resets state)

Any state → ResponseError  (worker encountered an error)
Any state → Shutdown        (either side initiates teardown)
```

## Request Payload

```
RequestPayload { version: u32, script_path_len: u32 }
followed by: script_path_len bytes of UTF-8 path
```

## Response Payloads

### Success (scene with multiple objects)

```
ResponsePayloadScene {
  version:          u32
  object_count:     u32
  op_count:         u32
  records_size:     u32   // total bytes of all SceneObjectRecord entries
  diagnostics_len:  u32
  object_table_size: u32
}
followed by: object_table_size bytes of SceneObjectRecord[] + name strings
followed by: op_count * OpRecordHeader + payloads
followed by: diagnostics_len bytes of UTF-8 diagnostics
```

### Error

```
ResponsePayloadError {
  version, error_code, phase, line, column: u32
  run_id: u64, duration_ms: u32
  file_len, stack_len, message_len: u32
}
followed by: file string + stack string + message string (UTF-8)
```

## Op Record Format

```
OpRecordHeader { opcode: u16, flags: u16, payload_len: u32 }
followed by: payload_len bytes of opcode-specific data
```

Op codes are defined in `OpCode` enum in `ipc_protocol.h`.

## Op Codes

| Code | Name | Kind |
|------|------|------|
| 1 | Sphere | 3D |
| 2 | Cube | 3D |
| 3 | Cylinder | 3D |
| 4 | Union | 3D |
| 5 | Subtract | 3D |
| 6 | Intersect | 3D |
| 7 | Translate | 3D |
| 8 | Rotate | 3D |
| 9 | Scale | 3D |
| 10 | Extrude | 3D |
| 11 | Revolve | 3D |
| 12 | Slice | 3D |
| 100 | CrossCircle | 2D |
| 101 | CrossSquare | 2D |
| 102 | CrossTranslate | 2D |
| 103 | CrossRotate | 2D |
| 104 | CrossRect | 2D |
| 105 | CrossPoint | 2D |
| 106 | CrossPolygons | 2D |
| 107 | CrossFillet | 2D |
| 108 | CrossOffsetClone | 2D |
| 109 | CrossPlane | 2D |
| 110 | CrossFilletCorners | 2D |
