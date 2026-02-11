const outerRadius = 20;
const beadRadius = 2;
const height = 40;
const twist = 90;

const { revolve, sphere, union, extrude } = Manifold;
const $2D = CrossSection;
// const {circle} = CrossSection;
// setMinCircularEdgeLength(0.1);

// const bead1 =
//     revolve(circle(beadRadius, 200).translate([outerRadius, 0]), 50, 90)
//         .add(sphere(beadRadius, 200).translate([outerRadius, 0, 0]))
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
const circle = $2D.circle(8, 200)
const square = $2D.square(15, true)
// const result = extrude(circle.subtract(square), 10, 200, 0);

vicad.addSketch(circle);
vicad.addSketch(square);


vicad.addToScene(extrude(circle, 10).intersect(extrude(square, 10)));
// vicad.addToScene(result, { id: "partA", name: "Part A" });
