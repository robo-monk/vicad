const { revolve, sphere, union, extrude } = Manifold;
const { polygon, rectangle, circle, point, polygons } = CrossSection;

function screw(radius: number, height: number, threadsPerRevolution: number) {
  console.log(radius, height, threadsPerRevolution);
  const threadProfile = polygon([
    [0, 0],
    [radius, 0],
    [radius, height],
  ]);
  // const thread = extrude(threadProfile, height);
  // return revolve(thread, 360 / threadsPerRevolution);
}

// const screw1 = screw(10, 10, 10);

rectangle(10, 10)