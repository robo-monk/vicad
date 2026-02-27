const $2D = CrossSection;
const { extrude } = Manifold;

const base = $2D.rectangle(80, 50, true);
const profile = base.filletCorners([
  { contour: 0, vertex: 0, radius: 4 },
  { contour: 0, vertex: 1, radius: 4 },
  { contour: 0, vertex: 2, radius: 10 },
]);

vicad.addSketch(profile, { name: "Per-Corner Fillet Profile" });
vicad.addToScene(extrude(profile, 8), { name: "Per-Corner Fillet Plate" });
