import type {
  CrossSection as VicadCrossSection,
  GLTFNode as VicadGLTFNode,
  Manifold as VicadManifold,
  Mesh as VicadMesh,
  Plane as VicadPlane,
  vicad as VicadApi,
} from "./worker/proxy-manifold";

declare global {
  const Manifold: typeof VicadManifold;
  const CrossSection: typeof VicadCrossSection;
  const GLTFNode: typeof VicadGLTFNode;
  const Mesh: typeof VicadMesh;
  const Plane: typeof VicadPlane;
  const XY: ReturnType<typeof VicadPlane.XY>;
  const XZ: ReturnType<typeof VicadPlane.XZ>;
  const YZ: ReturnType<typeof VicadPlane.YZ>;
  const vicad: typeof VicadApi;
}

export {};
