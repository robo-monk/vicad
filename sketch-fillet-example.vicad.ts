const $2D = CrossSection;
const { extrude } = Manifold;

const plate = $2D.rectangle(80, 50, true);
const cutout = $2D.rectangle(24, 16, true).translate(10, 0);

const profile = plate.subtract(cutout).fillet(4);

vicad.addSketch(profile, { name: "Filleted Profile" });
vicad.addToScene(extrude(profile, 8), { name: "Filleted Plate" });
