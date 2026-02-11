export const IPC_VERSION = 1;
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
  SANDBOX_VIOLATION: 2,
  SCRIPT_FAILURE: 3,
  ENCODE_FAILURE: 4,
  DECODE_FAILURE: 5,
  REPLAY_FAILURE: 6,
  TIMEOUT: 7,
  INTERNAL_ERROR: 8,
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
} as const;

export type OpCode = (typeof OP)[keyof typeof OP];
