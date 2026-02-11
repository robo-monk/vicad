import type {
  CrossSection as VicadCrossSection,
  GLTFNode as VicadGLTFNode,
  Manifold as VicadManifold,
  Mesh as VicadMesh,
  vicad as VicadApi,
} from "./worker/proxy-manifold";

declare global {
  const Manifold: typeof VicadManifold;
  const CrossSection: typeof VicadCrossSection;
  const GLTFNode: typeof VicadGLTFNode;
  const Mesh: typeof VicadMesh;
  const vicad: typeof VicadApi;
}

export {};
