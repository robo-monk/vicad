import { dirname, resolve } from "node:path";

function normalize(spec: string) {
  return spec.trim().replace(/^['"]|['"]$/g, "");
}

export function enforceScriptSandbox(src: string) {
  if (/import\s*\(/.test(src)) {
    throw new Error("SandboxViolation: dynamic import() is not allowed.");
  }
  if (/\bBun\.spawn\b/.test(src)) {
    throw new Error("SandboxViolation: Bun.spawn is not allowed.");
  }
  if (/\bprocess\./.test(src)) {
    throw new Error("SandboxViolation: process access is not allowed.");
  }
  if (/\bBun\.write\b/.test(src)) {
    throw new Error("SandboxViolation: Bun.write is not allowed.");
  }

  const staticImports = src.match(/import\s+[\s\S]*?\s+from\s+["'][^"']+["']/g) ?? [];
  for (const stmt of staticImports) {
    const m = stmt.match(/from\s+["']([^"']+)["']/);
    if (!m) continue;
    const spec = normalize(m[1]);
    if (spec === "manifold-3d/manifoldCAD") continue;
    if (spec.startsWith("http:") || spec.startsWith("https:") || spec.startsWith("node:")) {
      throw new Error(`SandboxViolation: disallowed import "${spec}".`);
    }
    const isLocal = spec.startsWith("./") || spec.startsWith("../") || spec.startsWith("/");
    if (!isLocal) throw new Error(`SandboxViolation: package imports are not allowed ("${spec}").`);
  }
}

export function rewriteManifoldImports(src: string, proxyModulePath: string, scriptPath: string) {
  const originalDir = dirname(resolve(scriptPath));
  const proxyAbs = resolve(proxyModulePath);
  let out = src.replace(
    /import\s*\{([^}]*)\}\s*from\s*['"]manifold-3d\/manifoldCAD['"]\s*;?/g,
    (_s, names: string) => `import {${names}} from "file://${proxyAbs}";`,
  );

  out =
    `import { Manifold, CrossSection, GLTFNode, Mesh, vicad } from "file://${proxyAbs}";\n` +
    `const __vicad_original_dir = ${JSON.stringify(originalDir)};\n` +
    out;
  return out;
}
