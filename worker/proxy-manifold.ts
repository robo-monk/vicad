import { NODE_KIND, OP } from "./ipc_protocol";
import { encodeOps, type EncodedOp } from "./op-encoder";

type Vec2Like = [number, number] | number[];
type Vec3Like = [number, number, number] | number[];

type Part = { t: "u32" | "u64" | "f64"; v: number | bigint };

type SceneEntry = {
  objectIdHash: bigint;
  rootKind: number;
  rootId: number;
  name: string;
};

enum PlaneKind {
  XY = 0,
  XZ = 1,
  YZ = 2,
}

const FNV64_OFFSET = 0xcbf29ce484222325n;
const FNV64_PRIME = 0x100000001b3n;
const U64_MASK = 0xffffffffffffffffn;

function fnv1a64(bytes: Uint8Array) {
  let h = FNV64_OFFSET;
  for (const b of bytes) {
    h ^= BigInt(b);
    h = (h * FNV64_PRIME) & U64_MASK;
  }
  return h;
}

function hashString64(s: string) {
  return fnv1a64(new TextEncoder().encode(s));
}

function hashCombine64(parts: bigint[]) {
  let h = FNV64_OFFSET;
  for (const p of parts) {
    let x = p & U64_MASK;
    for (let i = 0; i < 8; i++) {
      const b = Number(x & 0xffn);
      h ^= BigInt(b);
      h = (h * FNV64_PRIME) & U64_MASK;
      x >>= 8n;
    }
  }
  return h;
}

function makePayload(parts: Part[]) {
  let bytes = 0;
  for (const p of parts) bytes += p.t === "u32" ? 4 : 8;
  const out = new Uint8Array(bytes);
  const view = new DataView(out.buffer);
  let off = 0;
  for (const p of parts) {
    if (p.t === "u32") {
      view.setUint32(off, Number(p.v) >>> 0, true);
      off += 4;
    } else if (p.t === "u64") {
      view.setBigUint64(off, BigInt(p.v), true);
      off += 8;
    } else {
      view.setFloat64(off, Number(p.v), true);
      off += 8;
    }
  }
  return out.buffer;
}

function makePolygonsPayload(outId: number, contours: [number, number][][]) {
  let bytes = 8;
  for (const contour of contours) {
    bytes += 4;
    bytes += contour.length * 16;
  }
  const out = new Uint8Array(bytes);
  const view = new DataView(out.buffer);
  let off = 0;
  view.setUint32(off, outId >>> 0, true);
  off += 4;
  view.setUint32(off, contours.length >>> 0, true);
  off += 4;
  for (const contour of contours) {
    view.setUint32(off, contour.length >>> 0, true);
    off += 4;
    for (const [x, y] of contour) {
      view.setFloat64(off, x, true);
      off += 8;
      view.setFloat64(off, y, true);
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

function regularPolygonFromParallelDistance(sides: number, parallelDistance: number, center: boolean): [number, number][] {
  const n = Math.floor(Number(sides));
  const d = Number(parallelDistance);
  if (!Number.isFinite(n) || n < 3) {
    throw new Error("CrossSection.regularPolygon requires side count >= 3.");
  }
  if (!Number.isFinite(d) || d <= 0) {
    throw new Error("CrossSection.regularPolygon requires a positive parallel distance.");
  }
  const apothem = d * 0.5;
  const radius = apothem / Math.cos(Math.PI / n);
  const start = Math.PI * 0.5 - Math.PI / n;
  const pts: [number, number][] = [];
  let minX = Number.POSITIVE_INFINITY;
  let minY = Number.POSITIVE_INFINITY;
  for (let i = 0; i < n; i++) {
    const a = start + (2.0 * Math.PI * i) / n;
    const x = radius * Math.cos(a);
    const y = radius * Math.sin(a);
    pts.push([x, y]);
    if (x < minX) minX = x;
    if (y < minY) minY = y;
  }
  if (!center) {
    for (let i = 0; i < pts.length; i++) {
      const [x, y] = pts[i];
      pts[i] = [x - minX, y - minY];
    }
  }
  return pts;
}

function unsupported(name: string): never {
  throw new Error(`UnsupportedOperationError: ${name}`);
}

class Registry {
  private nextNodeId = 1;
  ops: EncodedOp[] = [];
  sceneEntries: SceneEntry[] = [];
  nodeDigest = new Map<number, bigint>();

  reset() {
    this.nextNodeId = 1;
    this.ops = [];
    this.sceneEntries = [];
    this.nodeDigest.clear();
  }

  allocNodeId() {
    return this.nextNodeId++;
  }

  push(opcode: number, payload: ArrayBuffer) {
    this.ops.push({ opcode, payload });
    const bytes = new Uint8Array(payload);
    if (bytes.byteLength >= 4) {
      const outId = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(0, true);
      const digest = hashCombine64([BigInt(opcode >>> 0), fnv1a64(bytes)]);
      this.nodeDigest.set(outId, digest);
    }
  }

  addSceneObject(rootKind: number, rootId: number, opts?: { id?: string; name?: string }) {
    const ordinal = this.sceneEntries.length;
    const userId = opts?.id?.trim() ?? "";
    const name = opts?.name?.trim() ?? "";

    const objectIdHash = userId
      ? hashString64(userId)
      : hashCombine64([
          BigInt(rootKind >>> 0),
          BigInt(rootId >>> 0),
          this.nodeDigest.get(rootId) ?? 0n,
          BigInt(ordinal >>> 0),
        ]);

    if (this.sceneEntries.some((s) => s.objectIdHash === objectIdHash)) {
      throw new Error(`SceneRegistrationError: duplicate object id hash 0x${objectIdHash.toString(16)}.`);
    }

    this.sceneEntries.push({ objectIdHash, rootKind, rootId, name });
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

function toCrossSectionRoot(value: unknown) {
  if (value instanceof CrossSection) {
    value.assertValid("CrossSection");
    return { rootKind: NODE_KIND.CROSS_SECTION, rootId: value.nodeId };
  }
  throw new Error("SceneRegistrationError: expected CrossSection.");
}

function toManifoldRoot(value: unknown) {
  if (value instanceof Manifold) {
    value.assertValid("Manifold");
    return { rootKind: NODE_KIND.MANIFOLD, rootId: value.nodeId };
  }
  if (value instanceof GLTFNode && value.manifold instanceof Manifold) {
    value.manifold.assertValid("Manifold");
    return { rootKind: NODE_KIND.MANIFOLD, rootId: value.manifold.nodeId };
  }
  if (value instanceof Mesh) {
    throw new Error("UnsupportedOperationError: Mesh values cannot be added to scene in IPC mode.");
  }
  throw new Error("SceneRegistrationError: expected Manifold or GLTFNode(manifold).");
}

function toAnyRoot(value: unknown) {
  if (value instanceof CrossSection) return toCrossSectionRoot(value);
  return toManifoldRoot(value);
}

const _crossSections = new Set<CrossSection>();
const _manifolds = new Set<Manifold>();

function applyCrossPlane(crossSection: CrossSection, planeKind: PlaneKind, planeOffset: number, opName: string) {
  crossSection.assertValid(opName);
  if (planeKind === PlaneKind.XY && planeOffset === 0) {
    return crossSection;
  }
  const out = reg.allocNodeId();
  reg.push(OP.CROSS_PLANE, makePayload([
    { t: "u32", v: out },
    { t: "u32", v: crossSection.nodeId },
    { t: "u32", v: planeKind },
    { t: "f64", v: planeOffset },
  ]));
  crossSection.consume(opName);
  return new CrossSection(out);
}

class PlaneHandle {
  readonly kind: PlaneKind;
  readonly distance: number;

  constructor(kind: PlaneKind, distance = 0) {
    if (!Number.isFinite(distance)) {
      throw new Error("Plane.offset requires a finite distance.");
    }
    this.kind = kind;
    this.distance = Number(distance);
  }

  offset(distance: number) {
    const d = Number(distance);
    if (!Number.isFinite(d)) {
      throw new Error("Plane.offset requires a finite distance.");
    }
    return new PlaneHandle(this.kind, this.distance + d);
  }

  private withPlane(crossSection: CrossSection, opName: string) {
    return applyCrossPlane(crossSection, this.kind, this.distance, opName);
  }

  rectangle(sizeOrWidth: number | Vec2Like = [1, 1], heightOrCenter?: number | boolean, centerArg = false) {
    return this.withPlane(CrossSection.rectangle(sizeOrWidth, heightOrCenter, centerArg), "Plane.rectangle");
  }

  square(size: number | Vec2Like = [1, 1], center = false) {
    return this.withPlane(CrossSection.square(size, center), "Plane.square");
  }

  circle(radius = 1) {
    return this.withPlane(CrossSection.circle(radius), "Plane.circle");
  }

  point(position: Vec2Like = [0, 0], radius = 0.1) {
    return this.withPlane(CrossSection.point(position, radius), "Plane.point");
  }

  polygons(contours: Vec2Like[][]) {
    return this.withPlane(CrossSection.polygons(contours), "Plane.polygons");
  }

  polygon(points: Vec2Like[]) {
    return this.withPlane(CrossSection.polygon(points), "Plane.polygon");
  }

  regularPolygon(sides: number, parallelDistance: number, center = true) {
    return this.withPlane(CrossSection.regularPolygon(sides, parallelDistance, center), "Plane.regularPolygon");
  }
}

export class Plane {
  static XY() { return new PlaneHandle(PlaneKind.XY, 0); }
  static XZ() { return new PlaneHandle(PlaneKind.XZ, 0); }
  static YZ() { return new PlaneHandle(PlaneKind.YZ, 0); }
}

export const XY = Plane.XY();
export const XZ = Plane.XZ();
export const YZ = Plane.YZ();

export class CrossSection {
  readonly nodeId: number;
  autoAddToScene = true;
  private valid = true;

  constructor(nodeId: number) {
    this.nodeId = nodeId;
    _crossSections.add(this);
  }

  assertValid(name = "CrossSection") {
    if (!this.valid) throw new Error(`InvalidObjectError: ${name} is invalidated.`);
  }

  private invalidate() {
    this.valid = false;
    this.autoAddToScene = false;
  }

  private derive(outNodeId: number, opName: string) {
    this.assertValid(opName);
    this.invalidate();
    return new CrossSection(outNodeId);
  }

  consume(opName: string) {
    this.assertValid(opName);
    this.invalidate();
  }

  clone() {
    this.assertValid("CrossSection.clone");
    const out = new CrossSection(this.nodeId);
    out.autoAddToScene = this.autoAddToScene;
    return out;
  }

  static circle(radius = 1) {
    if (arguments.length > 1) {
      throw new Error("CrossSection.circle only accepts (radius).");
    }
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_CIRCLE, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: radius },
      { t: "u32", v: 0 },
    ]));
    return new CrossSection(out);
  }

  static square(size: number | Vec2Like = [1, 1], center = false) {
    const [x, y] = typeof size === "number" ? [size, size] : vec2(size);
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_SQUARE, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: x },
      { t: "f64", v: y },
      { t: "u32", v: center ? 1 : 0 },
    ]));
    return new CrossSection(out);
  }

  static rectangle(sizeOrWidth: number | Vec2Like = [1, 1], heightOrCenter?: number | boolean, centerArg = false) {
    let x = 1;
    let y = 1;
    let center = centerArg;
    if (typeof sizeOrWidth === "number") {
      x = Number(sizeOrWidth);
      if (typeof heightOrCenter === "number") {
        y = Number(heightOrCenter);
      } else {
        y = Number(sizeOrWidth);
        if (typeof heightOrCenter === "boolean") center = heightOrCenter;
      }
    } else {
      [x, y] = vec2(sizeOrWidth);
      if (typeof heightOrCenter === "boolean") center = heightOrCenter;
    }
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_RECT, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: x },
      { t: "f64", v: y },
      { t: "u32", v: center ? 1 : 0 },
    ]));
    return new CrossSection(out);
  }

  static point(position: Vec2Like = [0, 0], radius = 0.1) {
    if (arguments.length > 2) {
      throw new Error("CrossSection.point only accepts (position, radius).");
    }
    if (!Array.isArray(position)) {
      throw new Error("CrossSection.point requires position as [x, y].");
    }
    const [x, y] = vec2(position);
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_POINT, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: x },
      { t: "f64", v: y },
      { t: "f64", v: radius },
      { t: "u32", v: 0 },
    ]));
    return new CrossSection(out);
  }

  static polygons(contours: Vec2Like[][]) {
    if (!Array.isArray(contours) || contours.length === 0) {
      throw new Error("CrossSection.polygons requires a non-empty array of contours.");
    }
    const normalized: [number, number][][] = [];
    for (const contour of contours) {
      if (!Array.isArray(contour) || contour.length < 3) {
        throw new Error("CrossSection.polygons requires each contour to have at least 3 points.");
      }
      const poly: [number, number][] = [];
      for (const p of contour) {
        const [x, y] = vec2(p);
        if (!Number.isFinite(x) || !Number.isFinite(y)) {
          throw new Error("CrossSection.polygons points must be finite numbers.");
        }
        poly.push([x, y]);
      }
      normalized.push(poly);
    }
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_POLYGONS, makePolygonsPayload(out, normalized));
    return new CrossSection(out);
  }

  static polygon(points: Vec2Like[]) {
    return CrossSection.polygons([points]);
  }

  static regularPolygon(sides: number, parallelDistance: number, center = true) {
    return CrossSection.polygon(regularPolygonFromParallelDistance(sides, parallelDistance, center));
  }

  translate(x: number | Vec2Like, y?: number) {
    const [tx, ty] = vec2(x, y);
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_TRANSLATE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: tx },
      { t: "f64", v: ty },
    ]));
    return this.derive(out, "CrossSection.translate");
  }

  rotate(degrees: number) {
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_ROTATE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: degrees },
    ]));
    return this.derive(out, "CrossSection.rotate");
  }

  fillet(radius: number) {
    const r = Number(radius);
    if (!Number.isFinite(r) || r < 0) {
      throw new Error("CrossSection.fillet requires a finite radius >= 0.");
    }
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_FILLET, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: r },
    ]));
    return this.derive(out, "CrossSection.fillet");
  }

  offsetClone(delta: number) {
    const d = Number(delta);
    if (!Number.isFinite(d)) {
      throw new Error("CrossSection.offsetClone requires a finite delta.");
    }
    const out = reg.allocNodeId();
    reg.push(OP.CROSS_OFFSET_CLONE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: d },
    ]));
    return this.derive(out, "CrossSection.offsetClone");
  }

  scale(): never {
    return unsupported("CrossSection.scale");
  }
}

export class Manifold {
  readonly nodeId: number;
  autoAddToScene = true;
  private valid = true;

  constructor(nodeId: number) {
    this.nodeId = nodeId;
    _manifolds.add(this);
  }

  assertValid(name = "Manifold") {
    if (!this.valid) throw new Error(`InvalidObjectError: ${name} is invalidated.`);
  }

  private invalidate() {
    this.valid = false;
    this.autoAddToScene = false;
  }

  private derive(outNodeId: number, opName: string) {
    this.assertValid(opName);
    this.invalidate();
    return new Manifold(outNodeId);
  }

  clone() {
    this.assertValid("Manifold.clone");
    const out = new Manifold(this.nodeId);
    out.autoAddToScene = this.autoAddToScene;
    return out;
  }

  static sphere(radius = 1) {
    if (arguments.length > 1) {
      throw new Error("Manifold.sphere only accepts (radius).");
    }
    const out = reg.allocNodeId();
    reg.push(OP.SPHERE, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: radius },
      { t: "u32", v: 0 },
    ]));
    return new Manifold(out);
  }

  static cube(size: number | Vec3Like = 1, center = false) {
    const [x, y, z] = typeof size === "number" ? [size, size, size] : vec3(size);
    const out = reg.allocNodeId();
    reg.push(OP.CUBE, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: x },
      { t: "f64", v: y },
      { t: "f64", v: z },
      { t: "u32", v: center ? 1 : 0 },
    ]));
    return new Manifold(out);
  }

  static cylinder(height: number, radiusLow: number, radiusHigh = -1, center = false) {
    if (arguments.length > 4) {
      throw new Error("Manifold.cylinder only accepts (height, radiusLow, radiusHigh?, center?).");
    }
    const out = reg.allocNodeId();
    reg.push(OP.CYLINDER, makePayload([
      { t: "u32", v: out },
      { t: "f64", v: height },
      { t: "f64", v: radiusLow },
      { t: "f64", v: radiusHigh },
      { t: "u32", v: 0 },
      { t: "u32", v: center ? 1 : 0 },
    ]));
    return new Manifold(out);
  }

  static union(items: Manifold[]) {
    if (!Array.isArray(items) || items.length === 0) throw new Error("Manifold.union requires a non-empty array.");
    for (const item of items) item.assertValid("Manifold.union");
    const out = reg.allocNodeId();
    const parts: Part[] = [{ t: "u32", v: out }, { t: "u32", v: items.length }];
    for (const item of items) parts.push({ t: "u32", v: item.nodeId });
    reg.push(OP.UNION, makePayload(parts));
    for (const item of items) item.invalidate();
    return new Manifold(out);
  }

  static extrude(crossSection: CrossSection, height: number, divisions = 0, twistDegrees = 0) {
    crossSection.assertValid("Manifold.extrude");
    const out = reg.allocNodeId();
    reg.push(OP.EXTRUDE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: crossSection.nodeId },
      { t: "f64", v: height },
      { t: "u32", v: divisions },
      { t: "f64", v: twistDegrees },
    ]));
    return new Manifold(out);
  }

  static revolve(crossSection: CrossSection, revolveDegrees = 360) {
    if (arguments.length > 2) {
      throw new Error("Manifold.revolve only accepts (crossSection, revolveDegrees?).");
    }
    crossSection.assertValid("Manifold.revolve");
    const out = reg.allocNodeId();
    reg.push(OP.REVOLVE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: crossSection.nodeId },
      { t: "u32", v: 0 },
      { t: "f64", v: revolveDegrees },
    ]));
    crossSection.consume("Manifold.revolve");
    return new Manifold(out);
  }

  add(other: Manifold) { return Manifold.union([this, other]); }

  subtract(other: Manifold) {
    this.assertValid("Manifold.subtract");
    other.assertValid("Manifold.subtract");
    const out = reg.allocNodeId();
    reg.push(OP.SUBTRACT, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "u32", v: other.nodeId },
    ]));
    this.invalidate();
    other.invalidate();
    return new Manifold(out);
  }

  intersect(other: Manifold) {
    this.assertValid("Manifold.intersect");
    other.assertValid("Manifold.intersect");
    const out = reg.allocNodeId();
    reg.push(OP.INTERSECT, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "u32", v: other.nodeId },
    ]));
    this.invalidate();
    other.invalidate();
    return new Manifold(out);
  }

  translate(x: number | Vec3Like, y?: number, z?: number) {
    const [tx, ty, tz] = vec3(x, y, z);
    const out = reg.allocNodeId();
    reg.push(OP.TRANSLATE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: tx },
      { t: "f64", v: ty },
      { t: "f64", v: tz },
    ]));
    return this.derive(out, "Manifold.translate");
  }

  rotate(x: number | Vec3Like, y?: number, z?: number) {
    const [rx, ry, rz] = vec3(x, y, z);
    const out = reg.allocNodeId();
    reg.push(OP.ROTATE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: rx },
      { t: "f64", v: ry },
      { t: "f64", v: rz },
    ]));
    return this.derive(out, "Manifold.rotate");
  }

  scale(x: number | Vec3Like, y?: number, z?: number) {
    const [sx, sy, sz] = vec3(x, y, z);
    const out = reg.allocNodeId();
    reg.push(OP.SCALE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: sx },
      { t: "f64", v: sy },
      { t: "f64", v: sz },
    ]));
    return this.derive(out, "Manifold.scale");
  }

  slice(height = 0) {
    this.assertValid("Manifold.slice");
    const out = reg.allocNodeId();
    reg.push(OP.SLICE, makePayload([
      { t: "u32", v: out },
      { t: "u32", v: this.nodeId },
      { t: "f64", v: height },
    ]));
    this.invalidate();
    return new CrossSection(out);
  }
}

export const vicad = {
  add(value: unknown, opts?: { id?: string; name?: string }) {
    const root = toAnyRoot(value);
    reg.addSceneObject(root.rootKind, root.rootId, opts);
    if (value instanceof CrossSection || value instanceof Manifold) {
      value.autoAddToScene = false;
    } else if (value instanceof GLTFNode && value.manifold instanceof Manifold) {
      value.manifold.autoAddToScene = false;
    }
  },
  addToScene(value: unknown, opts?: { id?: string; name?: string }) {
    const root = toManifoldRoot(value);
    reg.addSceneObject(root.rootKind, root.rootId, opts);
    if (value instanceof Manifold) value.autoAddToScene = false;
    if (value instanceof GLTFNode && value.manifold instanceof Manifold) value.manifold.autoAddToScene = false;
  },
  addSketch(value: unknown, opts?: { id?: string; name?: string }) {
    const root = toCrossSectionRoot(value);
    reg.addSceneObject(root.rootKind, root.rootId, opts);
    (value as CrossSection).autoAddToScene = false;
  },
  clearScene() {
    reg.sceneEntries = [];
  },
  listScene() {
    return reg.sceneEntries.map((entry) => ({
      objectIdHash: `0x${entry.objectIdHash.toString(16)}`,
      rootKind: entry.rootKind,
      rootId: entry.rootId,
      name: entry.name,
    }));
  },
};

export function __vicadBeginRun() {
  reg.reset();
  _crossSections.clear();
  _manifolds.clear();
}

export function __vicadEncodeScene() {
  const sceneEntries = reg.sceneEntries.slice();
  for (const crossSection of _crossSections) {
    if (crossSection.autoAddToScene) {
      reg.addSceneObject(NODE_KIND.CROSS_SECTION, crossSection.nodeId);
    }
  }
  for (const manifold of _manifolds) {
    if (manifold.autoAddToScene) {
      reg.addSceneObject(NODE_KIND.MANIFOLD, manifold.nodeId);
    }
  }
  const finalSceneEntries = reg.sceneEntries.slice();
  reg.sceneEntries = sceneEntries;

  if (finalSceneEntries.length === 0) {
    throw new Error("SceneRegistrationError: no scene entries were produced. Use autoAddToScene or vicad.add(...).");
  }
  return {
    opCount: reg.ops.length,
    records: encodeOps(reg.ops),
    sceneEntries: finalSceneEntries,
  };
}
