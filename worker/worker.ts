import { dlopen, FFIType, toArrayBuffer } from "bun:ffi";
import { resolve } from "node:path";
import { createConnection } from "node:net";

import {
  HEADER_OFFSETS,
  HEADER_SIZE,
  IPC_ERROR,
  IPC_ERROR_PHASE,
  IPC_MAGIC,
  IPC_STATE,
  IPC_VERSION,
  RESPONSE_OFFSETS,
} from "./ipc_protocol";
import {
  __vicadBeginRun,
  __vicadEncodeScene,
  CrossSection,
  GLTFNode,
  Manifold,
  Mesh,
  Plane,
  XY,
  XZ,
  YZ,
  vicad,
} from "./proxy-manifold";

const args = Bun.argv.slice(2);
function arg(name: string) {
  const i = args.indexOf(name);
  if (i < 0 || i + 1 >= args.length) return "";
  return args[i + 1];
}

const socketPath = arg("--socket");
const shmName = arg("--shm");
const shmSize = Number(arg("--size") || 0);
if (!socketPath || !shmName || !Number.isFinite(shmSize) || shmSize <= HEADER_SIZE) {
  throw new Error("Usage: bun worker/worker.ts --socket <path> --shm <name> --size <bytes>");
}

const libc = dlopen("/usr/lib/libSystem.B.dylib", {
  shm_open: { args: [FFIType.cstring, FFIType.i32, FFIType.i32], returns: FFIType.i32 },
  mmap: { args: [FFIType.ptr, FFIType.u64, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i64], returns: FFIType.ptr },
  close: { args: [FFIType.i32], returns: FFIType.i32 },
});

const O_RDWR = 0x0002;
const PROT_READ = 0x1;
const PROT_WRITE = 0x2;
const MAP_SHARED = 0x0001;

function cstr(value: string) {
  return new TextEncoder().encode(`${value}\0`);
}

const fd = libc.symbols.shm_open(cstr(shmName), O_RDWR, 0);
if (fd < 0) throw new Error(`shm_open failed for ${shmName}`);
const mapPtr = libc.symbols.mmap(0, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
if (Number(mapPtr) === -1) throw new Error("mmap failed");
const shared = new Uint8Array(toArrayBuffer(mapPtr, 0, shmSize));
const view = new DataView(shared.buffer, shared.byteOffset, shared.byteLength);
libc.symbols.close(fd);

function readAscii(offset: number, len: number) {
  return new TextDecoder().decode(shared.subarray(offset, offset + len));
}

function getU32(off: number) {
  return view.getUint32(off, true);
}
function setU32(off: number, v: number) {
  view.setUint32(off, v >>> 0, true);
}
function getU64(off: number) {
  return view.getBigUint64(off, true);
}
function setU64(off: number, v: bigint) {
  view.setBigUint64(off, v, true);
}

function headerCheck() {
  const magic = readAscii(HEADER_OFFSETS.magic, 8);
  const version = getU32(HEADER_OFFSETS.version);
  if (magic !== IPC_MAGIC) throw new Error(`Invalid shared memory magic: ${magic}`);
  if (version !== IPC_VERSION) throw new Error(`Shared memory version mismatch: ${version}`);
}

type ErrorDiagnostic = {
  code: number;
  phase: number;
  message: string;
  stack: string;
  file: string;
  line: number;
  column: number;
  runId: bigint;
  durationMs: number;
};

function writeErrorResponse(diag: ErrorDiagnostic) {
  const responseOffset = getU32(HEADER_OFFSETS.responseOffset);
  const capacity = getU32(HEADER_OFFSETS.capacityBytes);
  const message = new TextEncoder().encode(diag.message);
  const stack = new TextEncoder().encode(diag.stack);
  const file = new TextEncoder().encode(diag.file);
  const total = 44 + message.byteLength + stack.byteLength + file.byteLength;
  if (responseOffset + total > capacity) throw new Error("Error response exceeds shared memory size.");
  view.setUint32(responseOffset + 0, IPC_VERSION, true);
  view.setUint32(responseOffset + 4, diag.code >>> 0, true);
  view.setUint32(responseOffset + 8, diag.phase >>> 0, true);
  view.setUint32(responseOffset + 12, Math.max(0, diag.line) >>> 0, true);
  view.setUint32(responseOffset + 16, Math.max(0, diag.column) >>> 0, true);
  view.setBigUint64(responseOffset + 20, diag.runId, true);
  view.setUint32(responseOffset + 28, Math.max(0, Math.trunc(diag.durationMs)) >>> 0, true);
  view.setUint32(responseOffset + 32, file.byteLength >>> 0, true);
  view.setUint32(responseOffset + 36, stack.byteLength >>> 0, true);
  view.setUint32(responseOffset + 40, message.byteLength >>> 0, true);
  let off = responseOffset + 44;
  shared.set(file, off);
  off += file.byteLength;
  shared.set(stack, off);
  off += stack.byteLength;
  shared.set(message, off);
  setU32(HEADER_OFFSETS.responseLength, total);
  setU32(HEADER_OFFSETS.errorCode, diag.code);
  setU32(HEADER_OFFSETS.state, IPC_STATE.RESP_ERROR);
}

function writeSuccessResponseScene(
  objectCount: number,
  opCount: number,
  records: Uint8Array,
  objectTable: Uint8Array,
  namesBlob: Uint8Array,
) {
  const responseOffset = getU32(HEADER_OFFSETS.responseOffset);
  const capacity = getU32(HEADER_OFFSETS.capacityBytes);
  const headLen = RESPONSE_OFFSETS.sceneHeaderSize;
  const total = headLen + records.byteLength + objectTable.byteLength + namesBlob.byteLength;
  if (responseOffset + total > capacity) throw new Error("Success response exceeds shared memory size.");
  view.setUint32(responseOffset + RESPONSE_OFFSETS.sceneVersion, IPC_VERSION, true);
  view.setUint32(responseOffset + RESPONSE_OFFSETS.sceneObjectCount, objectCount >>> 0, true);
  view.setUint32(responseOffset + RESPONSE_OFFSETS.sceneOpCount, opCount >>> 0, true);
  view.setUint32(responseOffset + RESPONSE_OFFSETS.sceneRecordsSize, records.byteLength >>> 0, true);
  view.setUint32(responseOffset + RESPONSE_OFFSETS.sceneDiagnosticsLen, namesBlob.byteLength >>> 0, true);
  view.setUint32(responseOffset + RESPONSE_OFFSETS.sceneObjectTableSize, objectTable.byteLength >>> 0, true);

  let off = responseOffset + headLen;
  shared.set(records, off);
  off += records.byteLength;
  shared.set(objectTable, off);
  off += objectTable.byteLength;
  shared.set(namesBlob, off);
  setU32(HEADER_OFFSETS.responseLength, total);
  setU32(HEADER_OFFSETS.errorCode, IPC_ERROR.NONE);
  setU32(HEADER_OFFSETS.state, IPC_STATE.RESP_READY);
}

function requestScriptPath() {
  const reqOffset = getU32(HEADER_OFFSETS.requestOffset);
  const reqLen = getU32(HEADER_OFFSETS.requestLength);
  if (reqLen < 8) throw new Error("Request payload too short.");
  const reqVersion = view.getUint32(reqOffset, true);
  const pathLen = view.getUint32(reqOffset + 4, true);
  if (reqVersion !== IPC_VERSION) throw new Error("Request version mismatch.");
  if (8 + pathLen > reqLen) throw new Error("Request path is truncated.");
  const bytes = shared.subarray(reqOffset + 8, reqOffset + 8 + pathLen);
  return new TextDecoder().decode(bytes);
}

async function executeScript(scriptPath: string) {
  const abs = resolve(scriptPath);
  __vicadBeginRun();
  (globalThis as any).Manifold = Manifold;
  (globalThis as any).CrossSection = CrossSection;
  (globalThis as any).GLTFNode = GLTFNode;
  (globalThis as any).Mesh = Mesh;
  (globalThis as any).Plane = Plane;
  (globalThis as any).XY = XY;
  (globalThis as any).XZ = XZ;
  (globalThis as any).YZ = YZ;
  (globalThis as any).vicad = vicad;
  const loaded = await import(`file://${abs}?t=${Date.now()}`);
  if (loaded.default !== undefined) {
    throw new Error("SceneRegistrationError: scene mode uses side-effect registration only; default export is disabled.");
  }
  return __vicadEncodeScene();
}

function toErrCode(e: unknown) {
  const s = String(e);
  if (s.includes("UnsupportedOperationError")) return IPC_ERROR.ENCODE_FAILURE;
  return IPC_ERROR.SCRIPT_FAILURE;
}

function parseStackLocation(stack: string) {
  const line = stack.split("\n");
  for (const row of line) {
    const m = row.match(/\((file:\/\/[^:]+):(\d+):(\d+)\)/) ?? row.match(/at (file:\/\/[^:]+):(\d+):(\d+)/);
    if (m) {
      return {
        file: m[1].replace(/^file:\/\//, ""),
        line: Number(m[2]) || 0,
        column: Number(m[3]) || 0,
      };
    }
  }
  return { file: "", line: 0, column: 0 };
}

function makeDiag(error: unknown, phase: number, runId: bigint, startedAt: number): ErrorDiagnostic {
  const err = error instanceof Error ? error : new Error(String(error));
  const stack = err.stack ?? String(error);
  const loc = parseStackLocation(stack);
  let inferredPhase = phase;
  if (err.message.includes("Cannot find module") || err.message.includes("Unexpected")) {
    inferredPhase = IPC_ERROR_PHASE.SCRIPT_LOAD;
  } else if (err.message.includes("SceneRegistrationError") || err.message.includes("UnsupportedOperationError")) {
    inferredPhase = IPC_ERROR_PHASE.SCENE_ENCODE;
  }
  return {
    code: toErrCode(error),
    phase: inferredPhase,
    message: err.message || String(error),
    stack,
    file: loc.file,
    line: loc.line,
    column: loc.column,
    runId,
    durationMs: Date.now() - startedAt,
  };
}

headerCheck();

const sock = createConnection(socketPath);
let recv = "";
let shuttingDown = false;

sock.on("data", async (chunk: Buffer) => {
  recv += chunk.toString("utf8");
  for (;;) {
    const nl = recv.indexOf("\n");
    if (nl < 0) break;
    const line = recv.slice(0, nl).trim();
    recv = recv.slice(nl + 1);
    if (!line) continue;
    if (line === "SHUTDOWN") {
      setU32(HEADER_OFFSETS.state, IPC_STATE.SHUTDOWN);
      shuttingDown = true;
      sock.end();
      break;
    }
    if (!line.startsWith("RUN ")) continue;

    const seq = BigInt(line.slice(4).trim() || "0");
    const hdrSeq = getU64(HEADER_OFFSETS.requestSeq);
    if (seq !== hdrSeq) {
      const diag: ErrorDiagnostic = {
        code: IPC_ERROR.INVALID_REQUEST,
        phase: IPC_ERROR_PHASE.REQUEST_DECODE,
        message: "Sequence mismatch.",
        stack: "",
        file: "",
        line: 0,
        column: 0,
        runId: seq,
        durationMs: 0,
      };
      writeErrorResponse(diag);
      setU64(HEADER_OFFSETS.responseSeq, seq);
      sock.write(`ERROR ${seq}\n`);
      continue;
    }
    setU32(HEADER_OFFSETS.state, IPC_STATE.REQ_RUNNING);
    const startedAt = Date.now();
    console.error(`[vicad-worker] RUN_STARTED run_id=${seq.toString()}`);
    try {
      const path = requestScriptPath();
      const result = await executeScript(path);
      const entries = result.sceneEntries;
      const objectTable = new Uint8Array(entries.length * RESPONSE_OFFSETS.objectRecordSize);
      const tableView = new DataView(objectTable.buffer);
      const nameBytes: Uint8Array[] = [];
      let nameTotal = 0;
      for (let i = 0; i < entries.length; i++) {
        const e = entries[i];
        const recOff = i * RESPONSE_OFFSETS.objectRecordSize;
        const encodedName = new TextEncoder().encode(e.name ?? "");
        nameBytes.push(encodedName);
        nameTotal += encodedName.byteLength;
        tableView.setBigUint64(recOff + 0, BigInt(e.objectIdHash), true);
        tableView.setUint32(recOff + 8, e.rootKind >>> 0, true);
        tableView.setUint32(recOff + 12, e.rootId >>> 0, true);
        tableView.setUint32(recOff + 16, encodedName.byteLength >>> 0, true);
        tableView.setUint32(recOff + 20, 0, true);
      }
      const namesBlob = new Uint8Array(nameTotal);
      let nOff = 0;
      for (const nb of nameBytes) {
        namesBlob.set(nb, nOff);
        nOff += nb.byteLength;
      }
      writeSuccessResponseScene(entries.length, result.opCount, result.records, objectTable, namesBlob);
      setU64(HEADER_OFFSETS.responseSeq, seq);
      console.error(`[vicad-worker] RUN_DONE run_id=${seq.toString()} duration_ms=${Date.now() - startedAt}`);
      sock.write(`DONE ${seq}\n`);
    } catch (error) {
      const diag = makeDiag(error, IPC_ERROR_PHASE.SCRIPT_EXECUTE, seq, startedAt);
      writeErrorResponse(diag);
      setU64(HEADER_OFFSETS.responseSeq, seq);
      console.error(`[vicad-worker] RUN_FAILED run_id=${seq.toString()} duration_ms=${Date.now() - startedAt} code=${diag.code}`);
      sock.write(`ERROR ${seq}\n`);
    }
  }
});

sock.on("error", (error) => {
  if (!shuttingDown) {
    const diag: ErrorDiagnostic = {
      code: IPC_ERROR.INTERNAL_ERROR,
      phase: IPC_ERROR_PHASE.TRANSPORT,
      message: `Socket error: ${String(error)}`,
      stack: "",
      file: "",
      line: 0,
      column: 0,
      runId: 0n,
      durationMs: 0,
    };
    writeErrorResponse(diag);
  }
});
