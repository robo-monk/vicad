import type { OpCode } from "./ipc_protocol";

export type EncodedOp = {
  opcode: OpCode;
  payload: ArrayBuffer;
};

class Writer {
  private chunks: Uint8Array[] = [];
  private total = 0;

  append(buf: Uint8Array) {
    this.chunks.push(buf);
    this.total += buf.byteLength;
  }

  appendU32(v: number) {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, v >>> 0, true);
    this.append(b);
  }

  appendU16(v: number) {
    const b = new Uint8Array(2);
    new DataView(b.buffer).setUint16(0, v >>> 0, true);
    this.append(b);
  }

  appendF64(v: number) {
    const b = new Uint8Array(8);
    new DataView(b.buffer).setFloat64(0, Number(v), true);
    this.append(b);
  }

  toUint8Array() {
    const out = new Uint8Array(this.total);
    let off = 0;
    for (const c of this.chunks) {
      out.set(c, off);
      off += c.byteLength;
    }
    return out;
  }
}

export function payloadU32AndF64(outId: number, values: number[]) {
  const w = new Writer();
  w.appendU32(outId);
  for (const v of values) w.appendF64(v);
  return w.toUint8Array().buffer;
}

export function payloadU32(outId: number, values: number[]) {
  const w = new Writer();
  w.appendU32(outId);
  for (const v of values) w.appendU32(v);
  return w.toUint8Array().buffer;
}

export function payloadMixed(outId: number, f64Values: number[], u32Values: number[], u32First = false) {
  const w = new Writer();
  w.appendU32(outId);
  if (u32First) {
    for (const v of u32Values) w.appendU32(v);
    for (const v of f64Values) w.appendF64(v);
  } else {
    for (const v of f64Values) w.appendF64(v);
    for (const v of u32Values) w.appendU32(v);
  }
  return w.toUint8Array().buffer;
}

export function encodeOps(ops: EncodedOp[]) {
  const w = new Writer();
  for (const op of ops) {
    const payloadBytes = new Uint8Array(op.payload);
    w.appendU16(op.opcode);
    w.appendU16(0);
    w.appendU32(payloadBytes.byteLength);
    w.append(payloadBytes);
  }
  return w.toUint8Array();
}
