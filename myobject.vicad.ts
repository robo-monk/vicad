const { extrude } = Manifold;

// function extrudeMany(
//   n: number,
//   extrudes: (typeof CrossSection)["prototype"][],
// ) {
//   return Manifold.union(shapes.map((shape) => extrude(shape, n)));
// }

function solidify({
  depth,
  extrudes,
  subtractions,
}: {
  depth: number;
  extrudes: (typeof CrossSection)["prototype"][];
  subtractions?: (typeof CrossSection)["prototype"][];
}) {
  const x = Manifold.union(extrudes.map((e) => extrude(e, depth)));
  if (subtractions) {
    x.subtract(
      Manifold.union(
        subtractions.map((subtraction) => extrude(subtraction, depth)),
      ),
    );
  }
  return x;
}

const hinge = (depth: number, size: number = 10) =>
  solidify({
    depth,
    extrudes: [
      XZ.rectangle(size, size).translate(-size / 2, -size / 2),
      XZ.rectangle(size, size)
        .translate(0, -size / 2)
        .fillet(size / 2 - 0.1),
    ],
  });

hinge(5, 30).rotate(0, 180, 0).translate(30, 6);
hinge(6, 30);
hinge(5, 30).rotate(0, 180, 0).translate(30, -5);
