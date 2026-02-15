export const IPC_VERSION = 3;
export const IPC_MAGIC = "VCADIPC1";

export const HEADER_OFFSETS = {
  magic: 0,
  version: 8,
  capacityBytes: 12,
  requestSeq: 16,
  responseSeq: 24,
  requestOffset: 32,
  requestLength: 36,
  responseOffset: 40,
  responseLength: 44,
  state: 48,
  errorCode: 52,
} as const;

export const HEADER_SIZE = 60;

export const IPC_STATE = {
  IDLE: 0,
  REQ_READY: 1,
  REQ_RUNNING: 2,
  RESP_READY: 3,
  RESP_ERROR: 4,
  SHUTDOWN: 5,
} as const;

export const IPC_ERROR = {
  NONE: 0,
  INVALID_REQUEST: 1,
  SCRIPT_FAILURE: 2,
  ENCODE_FAILURE: 3,
  DECODE_FAILURE: 4,
  REPLAY_FAILURE: 5,
  TIMEOUT: 6,
  INTERNAL_ERROR: 7,
} as const;

export const IPC_ERROR_PHASE = {
  UNKNOWN: 0,
  REQUEST_DECODE: 1,
  SCRIPT_LOAD: 2,
  SCRIPT_EXECUTE: 3,
  SCENE_ENCODE: 4,
  RESPONSE_DECODE: 5,
  TRANSPORT: 6,
} as const;

export const NODE_KIND = {
  UNKNOWN: 0,
  MANIFOLD: 1,
  CROSS_SECTION: 2,
} as const;

export const OP = {
  SPHERE: 1,
  CUBE: 2,
  CYLINDER: 3,
  UNION: 4,
  SUBTRACT: 5,
  INTERSECT: 6,
  TRANSLATE: 7,
  ROTATE: 8,
  SCALE: 9,
  EXTRUDE: 10,
  REVOLVE: 11,
  SLICE: 12,
  CROSS_CIRCLE: 100,
  CROSS_SQUARE: 101,
  CROSS_TRANSLATE: 102,
  CROSS_ROTATE: 103,
  CROSS_RECT: 104,
  CROSS_POINT: 105,
  CROSS_POLYGONS: 106,
  CROSS_FILLET: 107,
  CROSS_OFFSET_CLONE: 108,
  CROSS_PLANE: 109,
} as const;

export type OpCode = (typeof OP)[keyof typeof OP];

export const RESPONSE_OFFSETS = {
  sceneVersion: 0,
  sceneObjectCount: 4,
  sceneOpCount: 8,
  sceneRecordsSize: 12,
  sceneDiagnosticsLen: 16,
  sceneObjectTableSize: 20,
  sceneHeaderSize: 24,
  objectRecordSize: 24,
} as const;
