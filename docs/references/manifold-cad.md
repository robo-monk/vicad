Manifold Class Reference
19 min. readView original
This library's internal representation of an oriented, 2-manifold, triangle mesh - a simple boundary-representation of a solid object. Use this class to store and operate on solids, and use MeshGL for input and output. More...

#include <manifold.h>

Public Types
enum class  	
Error {
  NoError , NonFiniteVertex , NotManifold , VertexOutOfBounds ,
  PropertiesWrongLength , MissingPositionProperties , MergeVectorsDifferentLengths , MergeIndexOutOfBounds ,
  TransformWrongLength , RunIndexWrongLength , FaceIDWrongLength , InvalidConstruction ,
  ResultTooLarge
}
Mesh ID
Details of the manifold's relation to its input meshes, for the purposes of reapplying mesh properties.

int 	
OriginalID () const
Manifold 	
AsOriginal () const
static uint32_t 	
ReserveIDs (uint32_t)
I/O
Self-contained mechanism for reading and writing high precision Manifold data. Write function creates special-purpose OBJ files, and Read function reads them in.

To work with a file, the caller should prepare the ifstream/ostream themselves, as follows:

Reading:

std::ifstream ifile;

ifile.open(filename);

if (ifile.is_open()) {

Manifold obj_m = Manifold::ReadOBJ(ifile);

ifile.close();

if (obj_m.Status() != Manifold::Error::NoError) {

std::cerr << "Failed reading " << filename << ":\n";

std::cerr << Manifold::ToString(ob_m.Status()) << "\n";

}

ifile.close();

}

Writing:

std::ofstream ofile;

ofile.open(filename);

if (ofile.is_open()) {

if (!m.WriteOBJ(ofile)) {

std::cerr << "Failed writing to " << filename << "\n";

}

}

ofile.close();

bool 	
WriteOBJ (std::ostream &stream) const
static Manifold 	
ReadOBJ (std::istream &stream)

Detailed Description
This library's internal representation of an oriented, 2-manifold, triangle mesh - a simple boundary-representation of a solid object. Use this class to store and operate on solids, and use MeshGL for input and output.

In addition to storing geometric data, a Manifold can also store an arbitrary number of vertex properties. These could be anything, e.g. normals, UV coordinates, colors, etc, but this library is completely agnostic. All properties are merely float values indexed by channel number. It is up to the user to associate channel numbers with meaning.

Manifold allows vertex properties to be shared for efficient storage, or to have multiple property verts associated with a single geometric vertex, allowing sudden property changes, e.g. at Boolean intersections, without sacrificing manifoldness.

Manifolds also keep track of their relationships to their inputs, via OriginalIDs and the faceIDs and transforms accessible through MeshGL. This allows object-level properties to be re-associated with the output after many operations, particularly useful for materials. Since separate object's properties are not mixed, there is no requirement that channels have consistent meaning between different inputs.


Constructor & Destructor Documentation

◆ Manifold() [1/3]

◆ Manifold() [2/3]
Convert a MeshGL into a Manifold, retaining its properties and merging only the positions according to the merge vectors. Will return an empty Manifold and set an Error Status if the result is not an oriented 2-manifold. Will collapse degenerate triangles and unnecessary vertices.

All fields are read, making this structure suitable for a lossless round-trip of data from GetMeshGL. For multi-material input, use ReserveIDs to set a unique originalID for each material, and sort the materials into triangle runs.

Parameters

◆ Manifold() [3/3]
Convert a MeshGL into a Manifold, retaining its properties and merging only the positions according to the merge vectors. Will return an empty Manifold and set an Error Status if the result is not an oriented 2-manifold. Will collapse degenerate triangles and unnecessary vertices.

All fields are read, making this structure suitable for a lossless round-trip of data from GetMeshGL. For multi-material input, use ReserveIDs to set a unique originalID for each material, and sort the materials into triangle runs.

Parameters

Member Function Documentation

◆ GetMeshGL()
MeshGL GetMeshGL	
(	
int	
normalIdx
 = 
-1	
)	
const
The most complete output of this library, returning a MeshGL that is designed to easily push into a renderer, including all interleaved vertex properties that may have been input. It also includes relations to all the input meshes that form a part of this result and the transforms applied to each.

Parameters
normalIdx	
If the original MeshGL inputs that formed this manifold had properties corresponding to normal vectors, you can specify the first of the three consecutive property channels forming the (x, y, z) normals, which will cause this output MeshGL to automatically update these normals according to the applied transforms and front/back side. normalIdx + 3 must be <= numProp, and all original MeshGLs must use the same channels for their normals.

◆ GetMeshGL64()
MeshGL64 GetMeshGL64	
(	
int	
normalIdx
 = 
-1	
)	
const
The most complete output of this library, returning a MeshGL that is designed to easily push into a renderer, including all interleaved vertex properties that may have been input. It also includes relations to all the input meshes that form a part of this result and the transforms applied to each.

Parameters
normalIdx	
If the original MeshGL inputs that formed this manifold had properties corresponding to normal vectors, you can specify the first of the three consecutive property channels forming the (x, y, z) normals, which will cause this output MeshGL to automatically update these normals according to the applied transforms and front/back side. normalIdx + 3 must be <= numProp, and all original MeshGLs must use the same channels for their normals.

◆ Decompose()
This operation returns a vector of Manifolds that are topologically disconnected. If everything is connected, the vector is length one, containing a copy of the original. It is the inverse operation of Compose().


◆ Compose()
Deprecated: Use BatchBoolean with OpType::Add instead.

Constructs a new manifold from a vector of other manifolds. This is a purely topological operation, so care should be taken to avoid creating overlapping results. It is the inverse operation of Decompose().

Parameters
manifolds	
A vector of Manifolds to lazy-union together.

◆ Tetrahedron()
Constructs a tetrahedron centered at the origin with one vertex at (1,1,1) and the rest at similarly symmetric points.


◆ Cube()
Manifold Cube	
(	
vec3	
size = vec3(1.0),
bool	
center = false )
static
Constructs a unit cube (edge lengths all one), by default in the first octant, touching the origin. If any dimensions in size are negative, or if all are zero, an empty Manifold will be returned.

Parameters
size	
The X, Y, and Z dimensions of the box.
center	
Set to true to shift the center to the origin.

◆ Cylinder()
Manifold Cylinder	
(	
double	
height,
double	
radiusLow,
double	
radiusHigh = -1.0,
int	
circularSegments = 0,
bool	
center = false )
static
A convenience constructor for the common case of extruding a circle. Can also form cones if both radii are specified.

Parameters
height	
Z-extent
radiusLow	
Radius of bottom circle. Must be non-negative. If zero, radiusHigh must be positive and a cone with apex at the bottom is created.
radiusHigh	
Radius of top circle. Can equal zero. Default is equal to radiusLow.
circularSegments	
How many line segments to use around the circle. Default is calculated by the static Defaults.
center	
Set to true to shift the center to the origin. Default is origin at the bottom.

◆ Sphere()
Manifold Sphere	
(	
double	
radius,
int	
circularSegments = 0 )
static
Constructs a geodesic sphere of a given radius.

Parameters
radius	
Radius of the sphere. Must be positive.
circularSegments	
Number of segments along its diameter. This number will always be rounded up to the nearest factor of four, as this sphere is constructed by refining an octahedron. This means there are a circle of vertices on all three of the axis planes. Default is calculated by the static Defaults.

◆ LevelSet()
Manifold LevelSet	
(	
std::function< double(vec3)>	
sdf,
Box	
bounds,
double	
edgeLength,
double	
level = 0,
double	
tolerance = -1,
bool	
canParallel = true )
static
Constructs a level-set manifold from the input Signed-Distance Function (SDF). This uses a form of Marching Tetrahedra (akin to Marching Cubes, but better for manifoldness). Instead of using a cubic grid, it uses a body-centered cubic grid (two shifted cubic grids). These grid points are snapped to the surface where possible to keep short edges from forming.

Parameters
sdf	
The signed-distance functor, containing this function signature: double operator()(vec3 point), which returns the signed distance of a given point in R^3. Positive values are inside, negative outside. There is no requirement that the function be a true distance, or even continuous.
bounds	
An axis-aligned box that defines the extent of the grid.
edgeLength	
Approximate maximum edge length of the triangles in the final result. This affects grid spacing, and hence has a strong effect on performance.
level	
Extract the surface at this value of your sdf; defaults to zero. You can inset your mesh by using a positive value, or outset it with a negative value.
tolerance	
Ensure each vertex is within this distance of the true surface. Defaults to -1, which will return the interpolated crossing-point based on the two nearest grid points. Small positive values will require more sdf evaluations per output vertex.
canParallel	
Parallel policies violate will crash language runtimes with runtime locks that expect to not be called back by unregistered threads. This allows bindings use LevelSet despite being compiled with MANIFOLD_PAR active.

◆ Slice()
Returns the cross section of this object parallel to the X-Y plane at the specified Z height, defaulting to zero. Using a height equal to the bottom of the bounding box will return the bottom faces, while using a height equal to the top of the bounding box will return empty.


◆ Project()
Returns polygons representing the projected outline of this object onto the X-Y plane. These polygons will often self-intersect, so it is recommended to run them through the positive fill rule of CrossSection to get a sensible result before using them.


◆ Extrude()
Manifold Extrude	
(	
const Polygons &	
crossSection,
double	
height,
int	
nDivisions = 0,
double	
twistDegrees = 0.0,
vec2	
scaleTop = vec2(1.0) )
static
Constructs a manifold from a set of polygons by extruding them along the Z-axis. Note that high twistDegrees with small nDivisions may cause self-intersection. This is not checked here and it is up to the user to choose the correct parameters.

Parameters
crossSection	
A set of non-overlapping polygons to extrude.
height	
Z-extent of extrusion.
nDivisions	
Number of extra copies of the crossSection to insert into the shape vertically; especially useful in combination with twistDegrees to avoid interpolation artifacts. Default is none.
twistDegrees	
Amount to twist the top crossSection relative to the bottom, interpolated linearly for the divisions in between.
scaleTop	
Amount to scale the top (independently in X and Y). If the scale is {0, 0}, a pure cone is formed with only a single vertex at the top. Note that scale is applied after twist. Default {1, 1}.

◆ Revolve()
Manifold Revolve	
(	
const Polygons &	
crossSection,
int	
circularSegments = 0,
double	
revolveDegrees = 360.0f )
static
Constructs a manifold from a set of polygons by revolving this cross-section around its Y-axis and then setting this as the Z-axis of the resulting manifold. If the polygons cross the Y-axis, only the part on the positive X side is used. Geometrically valid input will result in geometrically valid output.

Parameters
crossSection	
A set of non-overlapping polygons to revolve.
circularSegments	
Number of segments along its diameter. Default is calculated by the static Defaults.
revolveDegrees	
Number of degrees to revolve. Default is 360 degrees.

◆ Status()
Manifold::Error Status	
(		
)	
const
Returns the reason for an input Mesh producing an empty Manifold. This Status will carry on through operations like NaN propogation, ensuring an errored mesh doesn't get mysteriously lost. Empty meshes may still show NoError, for instance the intersection of non-overlapping meshes.


◆ IsEmpty()
Does the Manifold have any triangles?


◆ NumVert()
The number of vertices in the Manifold.


◆ NumEdge()
The number of edges in the Manifold.


◆ NumTri()
The number of triangles in the Manifold.


◆ NumProp()
The number of properties per vertex in the Manifold.


◆ NumPropVert()
size_t NumPropVert	
(		
)	
const
The number of property vertices in the Manifold. This will always be >= NumVert, as some physical vertices may be duplicated to account for different properties on different neighboring triangles.


◆ BoundingBox()
Returns the axis-aligned bounding box of all the Manifold's vertices.


◆ Genus()
The genus is a topological property of the manifold, representing the number of "handles". A sphere is 0, torus 1, etc. It is only meaningful for a single mesh, so it is best to call Decompose() first.


◆ GetTolerance()
double GetTolerance	
(		
)	
const
Returns the tolerance value of this Manifold. Triangles that are coplanar within tolerance tend to be merged and edges shorter than tolerance tend to be collapsed.


◆ SurfaceArea()
double SurfaceArea	
(		
)	
const
Returns the surface area of the manifold.


◆ Volume()
Returns the volume of the manifold.


◆ MinGap()
double MinGap	
(	
const Manifold &	
other,
double	
searchLength ) const
Returns the minimum gap between two manifolds. Returns a double between 0 and searchLength.

Parameters
other	
The other manifold to compute the minimum gap to.
searchLength	
The maximum distance to search for a minimum gap.

◆ OriginalID()
If this mesh is an original, this returns its meshID that can be referenced by product manifolds' MeshRelation. If this manifold is a product, this returns -1.


◆ AsOriginal()
This removes all relations (originalID, faceID, transform) to ancestor meshes and this new Manifold is marked an original. It also recreates faces

these don't get joined at boundaries where originalID changes, so the reset may allow triangles of flat faces to be further collapsed with Simplify().

◆ ReserveIDs()
uint32_t ReserveIDs	
(	
uint32_t	
n	
)	
static
Returns the first of n sequential new unique mesh IDs for marking sets of triangles that can be looked up after further operations. Assign to MeshGL.runOriginalID vector.


◆ Translate()
Move this Manifold in space. This operation can be chained. Transforms are combined and applied lazily.

Parameters
v	
The vector to add to every vertex.

◆ Scale()
Scale this Manifold in space. This operation can be chained. Transforms are combined and applied lazily.

Parameters
v	
The vector to multiply every vertex by per component.

◆ Rotate()
Manifold Rotate	
(	
double	
xDegrees,
double	
yDegrees = 0.0,
double	
zDegrees = 0.0 ) const
Applies an Euler angle rotation to the manifold, This operation can be chained. Transforms are combined and applied lazily.

We use degrees so that we can minimize rounding error, and eliminate it completely for any multiples of 90 degrees. Additionally, more efficient code paths are used to update the manifold when the transforms only rotate by multiples of 90 degrees.

From the reference frame of the model being rotated, rotations are applied in z-y'-x" order. That is yaw first, then pitch and finally roll.

From the global reference frame, a model will be rotated in x-y-z order. That is about the global X axis, then global Y axis, and finally global Z.

Parameters
xDegrees	
First rotation, degrees about the global X-axis.
yDegrees	
Second rotation, degrees about the global Y-axis.
zDegrees	
Third rotation, degrees about the global Z-axis.

◆ Mirror()
Mirror this Manifold over the plane described by the unit form of the given normal vector. If the length of the normal is zero, an empty Manifold is returned. This operation can be chained. Transforms are combined and applied lazily.

Parameters
normal	
The normal vector of the plane to be mirrored over

◆ Transform()
Transform this Manifold in space. The first three columns form a 3x3 matrix transform and the last is a translation vector. This operation can be chained. Transforms are combined and applied lazily.

Parameters
m	
The affine transform matrix to apply to all the vertices.

◆ Warp()
Manifold Warp	
(	
std::function< void(vec3 &)>	
warpFunc	
)	
const
This function does not change the topology, but allows the vertices to be moved according to any arbitrary input function. It is easy to create a function that warps a geometrically valid object into one which overlaps, but that is not checked here, so it is up to the user to choose their function with discretion.

Parameters
warpFunc	
A function that modifies a given vertex position.

◆ WarpBatch()
Same as Manifold::Warp but calls warpFunc with with a VecView which is roughly equivalent to std::span pointing to all vec3 elements to be modified in-place

Parameters
warpFunc	
A function that modifies multiple vertex positions.

◆ SetTolerance()
Manifold SetTolerance	
(	
double	
tolerance	
)	
const
Return a copy of the manifold with the set tolerance value. This performs mesh simplification when the tolerance value is increased.


◆ Simplify()
Manifold Simplify	
(	
double	
tolerance
 = 
0	
)	
const
Return a copy of the manifold simplified to the given tolerance, but with its actual tolerance value unchanged. If the tolerance is not given or is less than the current tolerance, the current tolerance is used for simplification. The result will contain a subset of the original verts and all surfaces will have moved by less than tolerance.


◆ Boolean()
The central operation of this library: the Boolean combines two manifolds into another by calculating their intersections and removing the unused portions. ε-valid inputs will produce ε-valid output. ε-invalid input may fail triangulation.

These operations are optimized to produce nearly-instant results if either input is empty or their bounding boxes do not overlap.

Parameters
second	
The other Manifold.
op	
The type of operation to perform.

◆ BatchBoolean()
Perform the given boolean operation on a list of Manifolds. In case of Subtract, all Manifolds in the tail are differenced from the head.


◆ operator+()

◆ operator+=()

◆ operator-()

◆ operator-=()
Shorthand for Boolean Difference assignment.


◆ operator^()

◆ operator^=()
Shorthand for Boolean Intersection assignment.


◆ Split()
Split cuts this manifold in two using the cutter manifold. The first result is the intersection, second is the difference. This is more efficient than doing them separately.

Parameters

◆ SplitByPlane()
Convenient version of Split() for a half-space.

Parameters
normal	
This vector is normal to the cutting plane and its length does not matter. The first result is in the direction of this vector, the second result is on the opposite side.
originOffset	
The distance of the plane from the origin in the direction of the normal vector.

◆ TrimByPlane()
Manifold TrimByPlane	
(	
vec3	
normal,
double	
originOffset ) const
Identical to SplitByPlane(), but calculating and returning only the first result.

Parameters
normal	
This vector is normal to the cutting plane and its length does not matter. The result is in the direction of this vector from the plane.
originOffset	
The distance of the plane from the origin in the direction of the normal vector.

◆ MinkowskiSum()
Compute the minkowski sum of this manifold with another. This corresponds to the morphological dilation of the manifold.

Note
Performance is best when using convex objects. For non-convex inputs, performance scales with the product of face counts, so keep face counts low.
Parameters
other	
The other manifold to minkowski sum to this one.

◆ MinkowskiDifference()
Subtract the sweep of the other manifold across this manifold's surface. This corresponds to the morphological erosion of the manifold.

Note
Performance is best when using convex objects. For non-convex inputs, performance scales with the product of face counts, so keep face counts low.
Parameters
other	
The other manifold to minkowski subtract from this one.

◆ SetProperties()
Manifold SetProperties	
(	
int	
numProp,
std::function< void(double *, vec3, const double *)>	
propFunc ) const
Create a new copy of this manifold with updated vertex properties by supplying a function that takes the existing position and properties as input. You may specify any number of output properties, allowing creation and removal of channels. Note: undefined behavior will result if you read past the number of input properties or write past the number of output properties.

If propFunc is a nullptr, this function will just set the channel to zeroes.

Parameters
numProp	
The new number of properties per vertex.
propFunc	
A function that modifies the properties of a given vertex.

◆ CalculateCurvature()
Manifold CalculateCurvature	
(	
int	
gaussianIdx,
int	
meanIdx ) const
Curvature is the inverse of the radius of curvature, and signed such that positive is convex and negative is concave. There are two orthogonal principal curvatures at any point on a manifold, with one maximum and the other minimum. Gaussian curvature is their product, while mean curvature is their sum. This approximates them for every vertex and assigns them as vertex properties on the given channels.

Parameters
gaussianIdx	
The property channel index in which to store the Gaussian curvature. An index < 0 will be ignored (stores nothing). The property set will be automatically expanded to include the channel index specified.
meanIdx	
The property channel index in which to store the mean curvature. An index < 0 will be ignored (stores nothing). The property set will be automatically expanded to include the channel index specified.

◆ CalculateNormals()
Manifold CalculateNormals	
(	
int	
normalIdx,
double	
minSharpAngle = 60 ) const
Fills in vertex properties for normal vectors, calculated from the mesh geometry. Flat faces composed of three or more triangles will remain flat.

Parameters
normalIdx	
The property channel in which to store the X values of the normals. The X, Y, and Z channels will be sequential. The property set will be automatically expanded such that NumProp will be at least normalIdx + 3.
minSharpAngle	
Any edges with angles greater than this value will remain sharp, getting different normal vector properties on each side of the edge. By default, no edges are sharp and all normals are shared. With a value of zero, the model is faceted and all normals match their triangle normals, but in this case it would be better not to calculate normals at all.

◆ Refine()
Increase the density of the mesh by splitting every edge into n pieces. For instance, with n = 2, each triangle will be split into 4 triangles. Quads will ignore their interior triangle bisector. These will all be coplanar (and will not be immediately collapsed) unless the Mesh/Manifold has halfedgeTangents specified (e.g. from the Smooth() constructor), in which case the new vertices will be moved to the interpolated surface according to their barycentric coordinates.

Parameters
n	
The number of pieces to split every edge into. Must be > 1.

◆ RefineToLength()
Manifold RefineToLength	
(	
double	
length	
)	
const
Increase the density of the mesh by splitting each edge into pieces of roughly the input length. Interior verts are added to keep the rest of the triangulation edges also of roughly the same length. If halfedgeTangents are present (e.g. from the Smooth() constructor), the new vertices will be moved to the interpolated surface according to their barycentric coordinates. Quads will ignore their interior triangle bisector.

Parameters
length	
The length that edges will be broken down to.

◆ RefineToTolerance()
Manifold RefineToTolerance	
(	
double	
tolerance	
)	
const
Increase the density of the mesh by splitting each edge into pieces such that any point on the resulting triangles is roughly within tolerance of the smoothly curved surface defined by the tangent vectors. This means tightly curving regions will be divided more finely than smoother regions. If halfedgeTangents are not present, the result will simply be a copy of the original. Quads will ignore their interior triangle bisector.

Parameters
tolerance	
The desired maximum distance between the faceted mesh produced and the exact smoothly curving surface. All vertices are exactly on the surface, within rounding error.

◆ SmoothByNormals()
Manifold SmoothByNormals	
(	
int	
normalIdx	
)	
const
Smooths out the Manifold by filling in the halfedgeTangent vectors. The geometry will remain unchanged until Refine or RefineToLength is called to interpolate the surface. This version uses the supplied vertex normal properties to define the tangent vectors. Faces of two coplanar triangles will be marked as quads, while faces with three or more will be flat.

Parameters
normalIdx	
The first property channel of the normals. NumProp must be at least normalIdx + 3. Any vertex where multiple normals exist and don't agree will result in a sharp edge.

◆ SmoothOut()
Manifold SmoothOut	
(	
double	
minSharpAngle = 60,
double	
minSmoothness = 0 ) const
Smooths out the Manifold by filling in the halfedgeTangent vectors. The geometry will remain unchanged until Refine or RefineToLength is called to interpolate the surface. This version uses the geometry of the triangles and pseudo-normals to define the tangent vectors. Faces of two coplanar triangles will be marked as quads.

Parameters
minSharpAngle	
degrees, default 60. Any edges with angles greater than this value will remain sharp. The rest will be smoothed to G1 continuity, with the caveat that flat faces of three or more triangles will always remain flat. With a value of zero, the model is faceted, but in this case there is no point in smoothing.
minSmoothness	
range: 0 - 1, default 0. The smoothness applied to sharp angles. The default gives a hard edge, while values > 0 will give a small fillet on these sharp edges. A value of 1 is equivalent to a minSharpAngle of 180 - all edges will be smooth.

◆ Smooth() [1/2]
Constructs a smooth version of the input mesh by creating tangents; this method will throw if you have supplied tangents with your mesh already. The actual triangle resolution is unchanged; use the Refine() method to interpolate to a higher-resolution curve.

By default, every edge is calculated for maximum smoothness (very much approximately), attempting to minimize the maximum mean Curvature magnitude. No higher-order derivatives are considered, as the interpolation is independent per triangle, only sharing constraints on their boundaries.

Parameters
meshGL	
input MeshGL.
sharpenedEdges	
If desired, you can supply a vector of sharpened halfedges, which should in general be a small subset of all halfedges. Order of entries doesn't matter, as each one specifies the desired smoothness (between zero and one, with one the default for all unspecified halfedges) and the halfedge index (3 * triangle index + [0,1,2] where 0 is the edge between triVert 0 and 1, etc).
At a smoothness value of zero, a sharp crease is made. The smoothness is interpolated along each edge, so the specified value should be thought of as an average. Where exactly two sharpened edges meet at a vertex, their tangents are rotated to be colinear so that the sharpened edge can be continuous. Vertices with only one sharpened edge are completely smooth, allowing sharpened edges to smoothly vanish at termination. A single vertex can be sharpened by sharping all edges that are incident on it, allowing cones to be formed.


◆ Smooth() [2/2]
Constructs a smooth version of the input mesh by creating tangents; this method will throw if you have supplied tangents with your mesh already. The actual triangle resolution is unchanged; use the Refine() method to interpolate to a higher-resolution curve.

By default, every edge is calculated for maximum smoothness (very much approximately), attempting to minimize the maximum mean Curvature magnitude. No higher-order derivatives are considered, as the interpolation is independent per triangle, only sharing constraints on their boundaries.

Parameters
meshGL64	
input MeshGL64.
sharpenedEdges	
If desired, you can supply a vector of sharpened halfedges, which should in general be a small subset of all halfedges. Order of entries doesn't matter, as each one specifies the desired smoothness (between zero and one, with one the default for all unspecified halfedges) and the halfedge index (3 * triangle index + [0,1,2] where 0 is the edge between triVert 0 and 1, etc).
At a smoothness value of zero, a sharp crease is made. The smoothness is interpolated along each edge, so the specified value should be thought of as an average. Where exactly two sharpened edges meet at a vertex, their tangents are rotated to be colinear so that the sharpened edge can be continuous. Vertices with only one sharpened edge are completely smooth, allowing sharpened edges to smoothly vanish at termination. A single vertex can be sharpened by sharping all edges that are incident on it, allowing cones to be formed.


◆ Hull() [1/3]
Compute the convex hull of this manifold.


◆ Hull() [2/3]
Compute the convex hull enveloping a set of manifolds.

Parameters
manifolds	
A vector of manifolds over which to compute a convex hull.

◆ Hull() [3/3]
Manifold Hull	
(	
const std::vector< vec3 > &	
pts	
)	
static
Compute the convex hull of a set of points. If the given points are fewer than 4, or they are all coplanar, an empty Manifold will be returned.

Parameters
pts	
A vector of 3-dimensional points over which to compute a convex hull.

◆ MatchesTriNormals()
bool MatchesTriNormals	
(		
)	
const
The triangle normal vectors are saved over the course of operations rather than recalculated to avoid rounding error. This checks that triangles still match their normal vectors within Precision().


◆ NumDegenerateTris()
size_t NumDegenerateTris	
(		
)	
const
The number of triangles that are colinear within Precision(). This library attempts to remove all of these, but it cannot always remove all of them without changing the mesh by too much.


◆ GetEpsilon()
Returns the epsilon value of this Manifold's vertices, which tracks the approximate rounding error over all the transforms and operations that have led to this state. This is the value of ε defining ε-valid.