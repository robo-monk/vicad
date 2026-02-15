import { describe, expect, it } from "bun:test";

import { NODE_KIND, OP } from "./ipc_protocol";
import { __vicadBeginRun, __vicadEncodeScene, CrossSection, Manifold, Plane, XY, XZ, vicad } from "./proxy-manifold";

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

describe("CrossSection.offsetClone", () => {
  it("encodes CROSS_OFFSET_CLONE with inId and delta", () => {
    __vicadBeginRun();
    const square = CrossSection.square(10);
    const offset = square.offsetClone(2.5);
    vicad.addSketch(offset, { name: "offset-test" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(2);
    expect(ops[1].opcode).toBe(OP.CROSS_OFFSET_CLONE);

    const payload = ops[1].payload;
    expect(payload.byteLength).toBe(16);
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const outId = view.getUint32(0, true);
    const inId = view.getUint32(4, true);
    const delta = view.getFloat64(8, true);
    expect(outId).toBe(offset.nodeId);
    expect(inId).toBe(square.nodeId);
    expect(delta).toBe(2.5);
  });

  it("accepts negative finite delta", () => {
    __vicadBeginRun();
    const square = CrossSection.square(5);
    const offset = square.offsetClone(-0.25);
    expect(offset.nodeId).not.toBe(square.nodeId);
  });

  it("rejects NaN and Infinity delta", () => {
    __vicadBeginRun();
    const square = CrossSection.square(5);
    expect(() => square.offsetClone(Number.NaN)).toThrow("CrossSection.offsetClone requires a finite delta.");
    expect(() => square.offsetClone(Number.NEGATIVE_INFINITY)).toThrow("CrossSection.offsetClone requires a finite delta.");
  });
});

describe("Canonical circular primitive signatures", () => {
  it("encodes sphere with auto profile-driven segments", () => {
    __vicadBeginRun();
    const sphere = Manifold.sphere(10);
    vicad.addToScene(sphere, { name: "sphere" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(1);
    expect(ops[0].opcode).toBe(OP.SPHERE);

    const view = new DataView(ops[0].payload.buffer, ops[0].payload.byteOffset, ops[0].payload.byteLength);
    expect(view.getUint32(12, true)).toBe(0);
  });

  it("encodes cylinder with auto profile-driven segments", () => {
    __vicadBeginRun();
    const cyl = Manifold.cylinder(20, 5, -1, false);
    vicad.addToScene(cyl, { name: "cylinder" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(1);
    expect(ops[0].opcode).toBe(OP.CYLINDER);

    const view = new DataView(ops[0].payload.buffer, ops[0].payload.byteOffset, ops[0].payload.byteLength);
    expect(view.getUint32(28, true)).toBe(0);
  });

  it("encodes revolve with auto profile-driven segments", () => {
    __vicadBeginRun();
    const profile = CrossSection.circle(5);
    const rev = Manifold.revolve(profile, 360);
    vicad.addToScene(rev, { name: "revolve" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(2);
    expect(ops[1].opcode).toBe(OP.REVOLVE);

    const view = new DataView(ops[1].payload.buffer, ops[1].payload.byteOffset, ops[1].payload.byteLength);
    expect(view.getUint32(8, true)).toBe(0);
  });

  it("rejects removed overloads and explicit segment arguments", () => {
    __vicadBeginRun();
    expect(() => (Manifold.sphere as unknown as (...args: unknown[]) => unknown)(10, 48)).toThrow(
      "Manifold.sphere only accepts (radius).",
    );
    expect(() => (Manifold.cylinder as unknown as (...args: unknown[]) => unknown)(20, 5, -1, 36, false)).toThrow(
      "Manifold.cylinder only accepts (height, radiusLow, radiusHigh?, center?).",
    );
    expect(() => {
      const profile = CrossSection.circle(5);
      (Manifold.revolve as unknown as (...args: unknown[]) => unknown)(profile, 22, 360);
    }).toThrow("Manifold.revolve only accepts (crossSection, revolveDegrees?).");
    expect(() => (CrossSection.circle as unknown as (...args: unknown[]) => unknown)(5, 24)).toThrow(
      "CrossSection.circle only accepts (radius).",
    );
    expect(() => (CrossSection.point as unknown as (...args: unknown[]) => unknown)(10, 10, 0.1, 12)).toThrow(
      "CrossSection.point only accepts (position, radius).",
    );
  });
});

describe("Auto-add lifecycle", () => {
  it("auto-adds only the latest derived cross section", () => {
    __vicadBeginRun();
    const square = CrossSection.square(10);
    const filleted = square.fillet(1);

    const scene = __vicadEncodeScene();
    expect(scene.sceneEntries.length).toBe(1);
    expect(scene.sceneEntries[0]?.rootKind).toBe(NODE_KIND.CROSS_SECTION);
    expect(scene.sceneEntries[0]?.rootId).toBe(filleted.nodeId);
  });

  it("invalidates consumed objects for follow-up ops", () => {
    __vicadBeginRun();
    const square = CrossSection.square(10);
    square.fillet(1);
    expect(() => square.rotate(15)).toThrow("InvalidObjectError: CrossSection.rotate is invalidated.");
  });

  it("clone keeps the parent valid", () => {
    __vicadBeginRun();
    const base = Manifold.cube(5);
    const moved = base.clone().translate(5, 0, 0);

    const scene = __vicadEncodeScene();
    expect(scene.sceneEntries.length).toBe(2);
    const ids = scene.sceneEntries.map((e) => e.rootId).sort((a, b) => a - b);
    expect(ids).toEqual([base.nodeId, moved.nodeId]);
  });

  it("extrude keeps both the source sketch and new manifold", () => {
    __vicadBeginRun();
    const sketch = CrossSection.rectangle(10, 10).fillet(1).translate(10, 0);
    const solid = Manifold.extrude(sketch, 10);

    const scene = __vicadEncodeScene();
    expect(scene.sceneEntries.length).toBe(2);

    const kinds = scene.sceneEntries.map((e) => e.rootKind);
    expect(kinds.includes(NODE_KIND.CROSS_SECTION)).toBe(true);
    expect(kinds.includes(NODE_KIND.MANIFOLD)).toBe(true);

    const ids = scene.sceneEntries.map((e) => e.rootId);
    expect(ids.includes(sketch.nodeId)).toBe(true);
    expect(ids.includes(solid.nodeId)).toBe(true);
  });
});

describe("Plane primitives", () => {
  it("XY.rectangle parity with CrossSection.rectangle", () => {
    __vicadBeginRun();
    const plain = CrossSection.rectangle(10, 10);
    const onXY = XY.rectangle(10, 10);
    vicad.addSketch(plain, { name: "plain" });
    vicad.addSketch(onXY, { name: "xy" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(2);
    expect(ops[0].opcode).toBe(OP.CROSS_RECT);
    expect(ops[1].opcode).toBe(OP.CROSS_RECT);
  });

  it("encodes CrossPlane for XZ offset circle", () => {
    __vicadBeginRun();
    const cs = XZ.offset(10).circle(1);
    vicad.addSketch(cs, { name: "xz-circle" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.length).toBe(2);
    expect(ops[0].opcode).toBe(OP.CROSS_CIRCLE);
    expect(ops[1].opcode).toBe(OP.CROSS_PLANE);

    const circleView = new DataView(ops[0].payload.buffer, ops[0].payload.byteOffset, ops[0].payload.byteLength);
    const circleOutId = circleView.getUint32(0, true);
    const view = new DataView(ops[1].payload.buffer, ops[1].payload.byteOffset, ops[1].payload.byteLength);
    expect(view.getUint32(4, true)).toBe(circleOutId);
    expect(view.getUint32(8, true)).toBe(1);
    expect(view.getFloat64(12, true)).toBe(10);
  });

  it("plane-generated cross section supports standard chaining", () => {
    __vicadBeginRun();
    const cs = Plane.YZ().offset(5).rectangle(10, 8).translate(2, 3).fillet(1);
    const solid = Manifold.extrude(cs, 4);
    vicad.addToScene(solid, { name: "plane-chain" });

    const scene = __vicadEncodeScene();
    const ops = decodeOps(scene.records);
    expect(ops.some((op) => op.opcode === OP.CROSS_PLANE)).toBe(true);
    expect(ops.some((op) => op.opcode === OP.CROSS_TRANSLATE)).toBe(true);
    expect(ops.some((op) => op.opcode === OP.CROSS_FILLET)).toBe(true);
    expect(ops.some((op) => op.opcode === OP.EXTRUDE)).toBe(true);
  });
});
