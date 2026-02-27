#ifndef VICAD_IPC_PROTOCOL_H_
#define VICAD_IPC_PROTOCOL_H_

#include <cstddef>
#include <cstdint>

namespace vicad {

static constexpr const char kIpcMagic[8] = {'V', 'C', 'A', 'D', 'I', 'P', 'C', '1'};
static constexpr uint32_t kIpcVersion = 3;
static constexpr size_t kDefaultShmSize = 100u * 1024u * 1024u;
static constexpr uint32_t kDefaultRequestOffset = 4096u;
static constexpr uint32_t kDefaultResponseOffset = 1024u * 1024u;

enum class IpcState : uint32_t {
  Idle = 0,
  RequestReady = 1,
  RequestRunning = 2,
  ResponseReady = 3,
  ResponseError = 4,
  Shutdown = 5,
};

enum class IpcErrorCode : uint32_t {
  None = 0,
  InvalidRequest = 1,
  ScriptFailure = 2,
  EncodeFailure = 3,
  DecodeFailure = 4,
  ReplayFailure = 5,
  Timeout = 6,
  InternalError = 7,
};

enum class IpcErrorPhase : uint32_t {
  Unknown = 0,
  RequestDecode = 1,
  ScriptLoad = 2,
  ScriptExecute = 3,
  SceneEncode = 4,
  ResponseDecode = 5,
  Transport = 6,
};

enum class OpCode : uint16_t {
  Sphere = 1,
  Cube = 2,
  Cylinder = 3,
  Union = 4,
  Subtract = 5,
  Intersect = 6,
  Translate = 7,
  Rotate = 8,
  Scale = 9,
  Extrude = 10,
  Revolve = 11,
  Slice = 12,
  CrossCircle = 100,
  CrossSquare = 101,
  CrossTranslate = 102,
  CrossRotate = 103,
  CrossRect = 104,
  CrossPoint = 105,
  CrossPolygons = 106,
  CrossFillet = 107,
  CrossOffsetClone = 108,
  CrossPlane = 109,
  CrossFilletCorners = 110,
};

enum class NodeKind : uint32_t {
  Unknown = 0,
  Manifold = 1,
  CrossSection = 2,
};

#pragma pack(push, 1)
struct SharedHeader {
  char magic[8];
  uint32_t version;
  uint32_t capacity_bytes;
  uint64_t request_seq;
  uint64_t response_seq;
  uint32_t request_offset;
  uint32_t request_length;
  uint32_t response_offset;
  uint32_t response_length;
  uint32_t state;
  uint32_t error_code;
  uint32_t reserved;
};

struct RequestPayload {
  uint32_t version;
  uint32_t script_path_len;
};

struct ResponsePayloadOk {
  uint32_t version;
  uint32_t root_kind;
  uint32_t root_id;
  uint32_t op_count;
  uint32_t records_size;
  uint32_t diagnostics_len;
};

struct SceneObjectRecord {
  uint64_t object_id_hash;
  uint32_t root_kind;
  uint32_t root_id;
  uint32_t name_len;
  uint32_t reserved;
};

struct ResponsePayloadScene {
  uint32_t version;
  uint32_t object_count;
  uint32_t op_count;
  uint32_t records_size;
  uint32_t diagnostics_len;
  uint32_t object_table_size;
};

struct ResponsePayloadError {
  uint32_t version;
  uint32_t error_code;
  uint32_t phase;
  uint32_t line;
  uint32_t column;
  uint64_t run_id;
  uint32_t duration_ms;
  uint32_t file_len;
  uint32_t stack_len;
  uint32_t message_len;
};

struct OpRecordHeader {
  uint16_t opcode;
  uint16_t flags;
  uint32_t payload_len;
};
#pragma pack(pop)

static_assert(sizeof(SharedHeader) == 60, "Unexpected SharedHeader size");

}  // namespace vicad

#endif  // VICAD_IPC_PROTOCOL_H_
