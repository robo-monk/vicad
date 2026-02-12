const outerRadius = 20;
const beadRadius = 2;
const height = 40;
const twist = 90;

const { revolve, sphere, union, extrude } = Manifold;
const $2D = CrossSection;
// const {circle} = CrossSection;
// setMinCircularEdgeLength(0.1);

// const bead1 =
//     revolve(circle(beadRadius).translate([outerRadius, 0]), 90)
//         .add(sphere(beadRadius).translate([outerRadius, 0, 0]))
//         .translate([0, -outerRadius, 0]);

// const beads = [];
// for (let i = 0; i < 3; i++) {
//   beads.push(bead1.rotate(0, 0, 120 * i));
// }
// const bead = union(beads);

// const auger = extrude(bead.slice(0), height, 250, twist);

// const result =
//     auger.add(bead).add(bead.translate(0, 0, height).rotate(0, 0, twist));

// export default result;
// const circle = $2D.circle(8, 500)
// const square = $2D.rectangle(15, 15).fillet(7);
// const square2 = square.offsetClone(10);
// const tri = $2D.polygon([
//     [0, 0], [3, 0], [1.5, 2],
//   ]);
// const point = $2D.point([10, 10], 0.01);
// const result = extrude(circle.subtract(square), 10, 200, 0);

// vicad.addSketch(circle);
// vicad.addSketch(square);
// vicad.addSketch(circle);
// vicad.addSketch(square2);
// vicad.addToScene(extrude(circle, 0.00000001).intersect(extrude(square, 0.00000001)));
// vicad.addSketch(tri);
// vicad.addToScene(extrude(tri, 10));

// const ringLike = $2D.polygons([
//     [[0,0],[4,0],[4,4],[0,4]],      // outer contour
//     [[10,1],[3,1],[3,3],[1,3]],      // inner contour
//   ]);
// vicad.addSketch(ringLike);
// vicad.addToScene(extrude(ringLike, 1));
// vicad.addSketch(point);

// vicad.addToScene(extrude(circle, 10).intersect(extrude(square, 10)));
// vicad.addToScene(result, { id: "partA", name: "Part A" });

// const beamProfile = $2D.rectangle(15, 15).fillet(1);

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
triangles.push(firstTri.rotate(180).translate(totalWidth - webThickness * 2, baseSize / 2 + webThickness));

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

  const tri2 = tri.translate(0);
  triangles.push(tri2.rotate(180).translate(baseSize / 2 - webThickness, baseSize / 2 + webThickness).translate(i * (baseSize + webThickness * 2), 0));
  triangles.push(tri.translate(i * (baseSize + webThickness * 2), 0));
}

const w = 10;
let beam = extrude(beamProfile, w);
for (const tri of triangles) {
  vicad.addSketch(tri);
  beam = beam.subtract(extrude(tri, w));
}



vicad.addToScene(beam);
vicad.addToScene(Manifold.sphere(10));
