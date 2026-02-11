import { describe, expect, it } from "bun:test";

import { OP } from "./ipc_protocol";
import { __vicadBeginRun, __vicadEncodeScene, CrossSection, vicad } from "./proxy-manifold";

type DecodedOp = {
  opcode: number;
  payload: Uint8Array;
};

function decodeOps(records: Uint8Array): DecodedOp[] {
  const out: DecodedOp[] = [];
  const view = new DataView(records.buffer, records.byteOffset, records.byteLength);
  let off = 0;
  while (off < records.byteLength) {
    if (off + 8 > records.byteLength) throw new Error("Truncated op header in records.");
    const opcode = view.getUint16(off + 0, true);
    const payloadLen = view.getUint32(off + 4, true);
    off += 8;
    if (off + payloadLen > records.byteLength) throw new Error("Truncated op payload in records.");
    out.push({
      opcode,
      payload: records.subarray(off, off + payloadLen),
    });
    off += payloadLen;
  }
  return out;
}

describe("CrossSection.fillet", () => {
  it("encodes CROSS_FILLET with inId and radius", () => {
    __vicadBeginRun();
    const square = CrossSection.square(10);
    const filleted = square.fillet(1.25);
    vicad.addSketch(filleted, { name: "fillet-test" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(2);
    expect(ops[1].opcode).toBe(OP.CROSS_FILLET);

    const payload = ops[1].payload;
    expect(payload.byteLength).toBe(16);
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const outId = view.getUint32(0, true);
    const inId = view.getUint32(4, true);
    const radius = view.getFloat64(8, true);
    expect(outId).toBe(filleted.nodeId);
    expect(inId).toBe(square.nodeId);
    expect(radius).toBe(1.25);
  });

  it("accepts radius 0 and still creates a new node", () => {
    __vicadBeginRun();
    const square = CrossSection.square(5);
    const filleted = square.fillet(0);
    expect(filleted.nodeId).not.toBe(square.nodeId);
  });

  it("rejects negative radius", () => {
    __vicadBeginRun();
    const square = CrossSection.square(5);
    expect(() => square.fillet(-0.01)).toThrow("CrossSection.fillet requires a finite radius >= 0.");
  });

  it("rejects NaN and Infinity radius", () => {
    __vicadBeginRun();
    const square = CrossSection.square(5);
    expect(() => square.fillet(Number.NaN)).toThrow("CrossSection.fillet requires a finite radius >= 0.");
    expect(() => square.fillet(Number.POSITIVE_INFINITY)).toThrow("CrossSection.fillet requires a finite radius >= 0.");
  });
});
