import { NODE_KIND, OP } from "./ipc_protocol";
import { encodeOps, type EncodedOp } from "./op-encoder";

type Vec2Like = [number, number] | number[];
type Vec3Like = [number, number, number] | number[];

type Part = { t: "u32" | "f64"; v: number };

function makePayload(parts: Part[]) {
  let bytes = 0;
  for (const p of parts) bytes += p.t === "u32" ? 4 : 8;
  const out = new Uint8Array(bytes);
  const view = new DataView(out.buffer);
  let off = 0;
  for (const p of parts) {
    if (p.t === "u32") {
      view.setUint32(off, p.v >>> 0, true);
      off += 4;
    } else {
      view.setFloat64(off, Number(p.v), true);
      off += 8;
    }
  }
  return out.buffer;
}

function vec3(xOrArr: number | Vec3Like, y?: number, z?: number): [number, number, number] {
  if (Array.isArray(xOrArr)) {
    return [Number(xOrArr[0] ?? 0), Number(xOrArr[1] ?? 0), Number(xOrArr[2] ?? 0)];
  }
  return [Number(xOrArr), Number(y ?? 0), Number(z ?? 0)];
}

function vec2(xOrArr: number | Vec2Like, y?: number): [number, number] {
  if (Array.isArray(xOrArr)) {
    return [Number(xOrArr[0] ?? 0), Number(xOrArr[1] ?? 0)];
  }
  return [Number(xOrArr), Number(y ?? 0)];
}

function unsupported(name: string): never {
  throw new Error(`UnsupportedOperationError: ${name}`);
}

class Registry {
  private nextNodeId = 1;
  ops: EncodedOp[] = [];

  reset() {
    this.nextNodeId = 1;
    this.ops = [];
  }

  allocNodeId() {
    return this.nextNodeId++;
  }

  push(opcode: number, payload: ArrayBuffer) {
    this.ops.push({ opcode, payload });
  }
}

const reg = new Registry();

export class Mesh {}

export class GLTFNode {
  manifold: Manifold | null;
  constructor(manifold: Manifold | null = null) {
    this.manifold = manifold;
  }
}

export class CrossSection {
  readonly nodeId: number;
  constructor(nodeId: number) {
    this.nodeId = nodeId;
  }

  static circle(radius = 1, circularSegments = 0) {
    const out = reg.allocNodeId();
    reg.push(
      OP.CROSS_CIRCLE,
      makePayload([
        { t: "u32", v: out },
        { t: "f64", v: radius },
        { t: "u32", v: circularSegments },
      ]),
    );
    return new CrossSection(out);
  }

  static square(size: number | Vec2Like = [1, 1], center = false) {
    const [x, y] = typeof size === "number" ? [size, size] : vec2(size);
    const out = reg.allocNodeId();
    reg.push(
      OP.CROSS_SQUARE,
      makePayload([
        { t: "u32", v: out },
        { t: "f64", v: x },
        { t: "f64", v: y },
        { t: "u32", v: center ? 1 : 0 },
      ]),
    );
    return new CrossSection(out);
  }

  translate(x: number | Vec2Like, y?: number) {
    const [tx, ty] = vec2(x, y);
    const out = reg.allocNodeId();
    reg.push(
      OP.CROSS_TRANSLATE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "f64", v: tx },
        { t: "f64", v: ty },
      ]),
    );
    return new CrossSection(out);
  }

  rotate(degrees: number) {
    const out = reg.allocNodeId();
    reg.push(
      OP.CROSS_ROTATE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "f64", v: degrees },
      ]),
    );
    return new CrossSection(out);
  }

  scale(): never {
    return unsupported("CrossSection.scale");
  }
}

export class Manifold {
  readonly nodeId: number;
  constructor(nodeId: number) {
    this.nodeId = nodeId;
  }

  static sphere(radius = 1, circularSegments = 0) {
    const out = reg.allocNodeId();
    reg.push(
      OP.SPHERE,
      makePayload([
        { t: "u32", v: out },
        { t: "f64", v: radius },
        { t: "u32", v: circularSegments },
      ]),
    );
    return new Manifold(out);
  }

  static cube(size: number | Vec3Like = 1, center = false) {
    const [x, y, z] = typeof size === "number" ? [size, size, size] : vec3(size);
    const out = reg.allocNodeId();
    reg.push(
      OP.CUBE,
      makePayload([
        { t: "u32", v: out },
        { t: "f64", v: x },
        { t: "f64", v: y },
        { t: "f64", v: z },
        { t: "u32", v: center ? 1 : 0 },
      ]),
    );
    return new Manifold(out);
  }

  static cylinder(height: number, radiusLow: number, radiusHigh = -1, circularSegments = 0, center = false) {
    const out = reg.allocNodeId();
    reg.push(
      OP.CYLINDER,
      makePayload([
        { t: "u32", v: out },
        { t: "f64", v: height },
        { t: "f64", v: radiusLow },
        { t: "f64", v: radiusHigh },
        { t: "u32", v: circularSegments },
        { t: "u32", v: center ? 1 : 0 },
      ]),
    );
    return new Manifold(out);
  }

  static union(items: Manifold[]) {
    if (!Array.isArray(items) || items.length === 0) throw new Error("Manifold.union requires a non-empty array.");
    const out = reg.allocNodeId();
    const parts: Part[] = [{ t: "u32", v: out }, { t: "u32", v: items.length }];
    for (const item of items) parts.push({ t: "u32", v: item.nodeId });
    reg.push(OP.UNION, makePayload(parts));
    return new Manifold(out);
  }

  static extrude(crossSection: CrossSection, height: number, divisions = 0, twistDegrees = 0) {
    const out = reg.allocNodeId();
    reg.push(
      OP.EXTRUDE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: crossSection.nodeId },
        { t: "f64", v: height },
        { t: "u32", v: divisions },
        { t: "f64", v: twistDegrees },
      ]),
    );
    return new Manifold(out);
  }

  static revolve(crossSection: CrossSection, circularSegments = 0, revolveDegrees = 360) {
    const out = reg.allocNodeId();
    reg.push(
      OP.REVOLVE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: crossSection.nodeId },
        { t: "u32", v: circularSegments },
        { t: "f64", v: revolveDegrees },
      ]),
    );
    return new Manifold(out);
  }

  add(other: Manifold) {
    return Manifold.union([this, other]);
  }

  subtract(other: Manifold) {
    const out = reg.allocNodeId();
    reg.push(
      OP.SUBTRACT,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "u32", v: other.nodeId },
      ]),
    );
    return new Manifold(out);
  }

  intersect(other: Manifold) {
    const out = reg.allocNodeId();
    reg.push(
      OP.INTERSECT,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "u32", v: other.nodeId },
      ]),
    );
    return new Manifold(out);
  }

  translate(x: number | Vec3Like, y?: number, z?: number) {
    const [tx, ty, tz] = vec3(x, y, z);
    const out = reg.allocNodeId();
    reg.push(
      OP.TRANSLATE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "f64", v: tx },
        { t: "f64", v: ty },
        { t: "f64", v: tz },
      ]),
    );
    return new Manifold(out);
  }

  rotate(x: number | Vec3Like, y?: number, z?: number) {
    const [rx, ry, rz] = vec3(x, y, z);
    const out = reg.allocNodeId();
    reg.push(
      OP.ROTATE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "f64", v: rx },
        { t: "f64", v: ry },
        { t: "f64", v: rz },
      ]),
    );
    return new Manifold(out);
  }

  scale(x: number | Vec3Like, y?: number, z?: number) {
    const [sx, sy, sz] = vec3(x, y, z);
    const out = reg.allocNodeId();
    reg.push(
      OP.SCALE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "f64", v: sx },
        { t: "f64", v: sy },
        { t: "f64", v: sz },
      ]),
    );
    return new Manifold(out);
  }

  slice(height = 0) {
    const out = reg.allocNodeId();
    reg.push(
      OP.SLICE,
      makePayload([
        { t: "u32", v: out },
        { t: "u32", v: this.nodeId },
        { t: "f64", v: height },
      ]),
    );
    return new CrossSection(out);
  }
}

export function __vicadBeginRun() {
  reg.reset();
}

export function __vicadEncodeDefault(value: unknown) {
  if (value instanceof Manifold) {
    return {
      rootKind: NODE_KIND.MANIFOLD,
      rootId: value.nodeId,
      opCount: reg.ops.length,
      records: encodeOps(reg.ops),
    };
  }
  if (value instanceof GLTFNode && value.manifold instanceof Manifold) {
    return {
      rootKind: NODE_KIND.MANIFOLD,
      rootId: value.manifold.nodeId,
      opCount: reg.ops.length,
      records: encodeOps(reg.ops),
    };
  }
  if (value instanceof Mesh) {
    throw new Error("UnsupportedOperationError: default export of Mesh is not supported in IPC mode.");
  }
  throw new Error("Default export must resolve to Manifold or GLTFNode(manifold).");
}
