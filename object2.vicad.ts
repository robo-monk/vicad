const outerRadius = 20;
const beadRadius = 2;
const height = 40;
const twist = 90;

const { revolve, sphere, union, extrude } = Manifold;
const $2D = CrossSection;
const baseSize = 18;
const webThickness = 2;
const filletRadius = 0.8;
const iterations = 4;

const triangles = [];

const firstTri = $2D
  .polygon([
    [-baseSize / 2, 0],
    [0, 0],
    [-baseSize / 2, baseSize / 2],
  ])
  .fillet(filletRadius)
  .translate(-2 * webThickness, 0);

// vicad.addSketch(firstTri);
triangles.push(firstTri);

const totalWidth = iterations * (baseSize + webThickness / 2);
// vicad.addSketch(
//   firstTri
//     .rotate(180)
//     .translate(totalWidth - webThickness * 2, baseSize / 2 + webThickness),
// );
triangles.push(
  firstTri
    .clone()
    .rotate(180)
    .translate(totalWidth - webThickness * 2, baseSize / 2 + webThickness),
);

const beamProfile = $2D
  .rectangle(
    totalWidth + baseSize + webThickness * 5,
    baseSize / 2 + 3 * webThickness,
  )
  .translate(-baseSize / 2 - webThickness * 3.5, -1 * webThickness)
  .fillet(filletRadius);

for (let i = 0; i < iterations; i++) {
  const tri = $2D
    .polygon([
      [0, 0],
      [baseSize, 0],
      [baseSize / 2, baseSize / 2],
    ])
    .fillet(filletRadius);

  const tri2 = tri.clone().translate(0);
  triangles.push(
    tri2
      .rotate(180)
      .translate(baseSize / 2 - webThickness, baseSize / 2 + webThickness)
      .translate(i * (baseSize + webThickness * 2), 0),
  );
  triangles.push(tri.translate(i * (baseSize + webThickness * 2), 0));
}

const w = 10;
let beam = extrude(beamProfile, w);
for (const tri of triangles) {
  vicad.addSketch(tri);
  beam = beam.subtract(extrude(tri, w));
}

// vicad.addToScene(beam);
// vicad.addToScene(Manifold.sphere(10));
