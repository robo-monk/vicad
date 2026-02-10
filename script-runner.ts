import * as CAD from "manifold-3d/manifoldCAD";
import { resolve } from "node:path";

const [, , scriptPath, outPath] = process.argv;
if (!scriptPath || !outPath) {
  console.error("Usage: bun script-runner.ts <scriptPath> <outPath>");
  process.exit(2);
}

const writeMeshBinary = async (mesh: any, path: string) => {
  const numProp = Number(mesh.numProp ?? 3);
  const vertProperties = Float32Array.from(mesh.vertProperties ?? []);
  const triVerts = Uint32Array.from(mesh.triVerts ?? []);

  if (!Number.isFinite(numProp) || numProp < 3) {
    throw new Error(`Invalid mesh.numProp: ${numProp}`);
  }
  if (vertProperties.length % numProp !== 0) {
    throw new Error(
      `vertProperties length (${vertProperties.length}) is not divisible by numProp (${numProp})`
    );
  }
  if (triVerts.length % 3 !== 0) {
    throw new Error(`triVerts length (${triVerts.length}) is not divisible by 3`);
  }

  const headerBytes = 8 + 4 + 4 + 4;
  const totalBytes =
    headerBytes + vertProperties.byteLength + triVerts.byteLength;
  const buf = new ArrayBuffer(totalBytes);
  const view = new DataView(buf);
  const out = new Uint8Array(buf);

  const magic = "VCADMSH1";
  for (let i = 0; i < magic.length; ++i) out[i] = magic.charCodeAt(i);

  let offset = 8;
  view.setUint32(offset, numProp, true);
  offset += 4;
  view.setUint32(offset, vertProperties.length, true);
  offset += 4;
  view.setUint32(offset, triVerts.length, true);
  offset += 4;

  out.set(new Uint8Array(vertProperties.buffer), offset);
  offset += vertProperties.byteLength;
  out.set(new Uint8Array(triVerts.buffer), offset);

  await Bun.write(path, out);
};

const normalizeToMesh = (value: any) => {
  if (value instanceof CAD.Manifold) {
    return value.getMesh();
  }
  if (value instanceof CAD.Mesh) {
    return value;
  }
  if (value instanceof CAD.GLTFNode) {
    if (value.manifold instanceof CAD.Manifold) {
      return value.manifold.getMesh();
    }
    throw new Error("GLTFNode default export must have node.manifold set.");
  }
  throw new Error(
    "Default export must be a Manifold, Mesh, or GLTFNode with node.manifold."
  );
};

try {
  const scriptAbsPath = resolve(scriptPath);
  const outAbsPath = resolve(outPath);
  const src = await Bun.file(scriptAbsPath).text();
  const wrapped =
    `import { Mesh, Manifold, GLTFNode } from \"manifold-3d/manifoldCAD\";\n` +
    `${src}\n`;

  const tempPath = `${outAbsPath}.tmp.${Date.now()}.mjs`;
  await Bun.write(tempPath, wrapped);

  let loaded: any;
  try {
    loaded = await import(`file://${tempPath}?t=${Date.now()}`);
  } finally {
    try {
      await Bun.file(tempPath).delete();
    } catch {
      // Best-effort cleanup.
    }
  }

  const mesh = normalizeToMesh(loaded.default);
  await writeMeshBinary(mesh, outAbsPath);
} catch (error) {
  const message = error instanceof Error ? `${error.message}\n${error.stack ?? ""}` : String(error);
  console.error(message);
  process.exit(1);
}
