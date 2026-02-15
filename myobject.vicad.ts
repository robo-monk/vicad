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

const hingeProfiles = (size: number = 10) => [
  XZ.rectangle(size, size).translate(-size / 2, -size / 2),
  XZ.rectangle(size, size)
    .translate(0, -size / 2)
    .fillet(size / 2 - 0.1),
];

const hinge = (depth: number, size: number = 10) =>
  solidify({
    depth,
    extrudes: hingeProfiles(size),
  });

const outerHeight = 5;
const middleHeight = 6;

hinge(outerHeight, 30).rotate(0, 180, 0).translate(30, middleHeight);
hinge(middleHeight, 30);
hinge(outerHeight, 30).rotate(0, 180, 0).translate(30, -outerHeight);

const totalHeight = outerHeight + middleHeight + outerHeight;

extrude(
  YZ.offset(30 + 30 / 2)
    .rectangle(totalHeight, 30)
    .translate(-outerHeight, -30 / 2),
  10,
);

XZ.offset(middleHeight).rectangle(totalHeight, 30).translate(-outerHeight, -30 / 2);