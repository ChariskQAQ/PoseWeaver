#pragma once
#include "core/pose.h"

namespace rig {

// ---------------------------------------------------------------- creation

// Turn a region attachment into a cols x rows grid mesh covering the image. When `rgba`
// (width*height*4 bytes) is given, cells whose pixels are all at or below
// `alphaThreshold` are dropped. Returns false if nothing remains.
bool convertRegionToMesh(Attachment& att, const Image& img, const unsigned char* rgba,
                         int cols, int rows, unsigned char alphaThreshold = 8);

// Outer outline of the largest opaque area (image pixels, x right / y down), simplified to
// within `tolerance` px and grown by `padding` px. Empty if nothing is opaque.
std::vector<Vec2> traceAlphaOutline(const unsigned char* rgba, int width, int height, unsigned char threshold,
                                    float tolerance, float padding);

// Affine map from image pixels (x right, y down, origin top-left) to slot-bone space in the
// setup pose. Regions use their transform; meshes are fitted to their vertices and uvs.
Affine imageToLocal(const Skeleton& sk, const Slot& slot, const Attachment& att, const Image& img);

// Rebuilds `att` (region or mesh) as an unweighted mesh: `outlinePx` becomes the hull, and
// interior points are added on a grid with `spacing` px (0 = none).
bool buildMeshFromOutline(const Skeleton& sk, const Slot& slot, Attachment& att, const Image& img,
                          const std::vector<Vec2>& outlinePx, float spacing);

// ---------------------------------------------------------------- topology

// Constrained Delaunay triangulation: points[0..hullLength) is a simple polygon in order (either
// winding), the rest are interior points. Points outside the polygon are ignored.
bool triangulate(const std::vector<Vec2>& points, int hullLength, std::vector<int>& triangles);
// Convex hull (counter-clockwise) indices of a point set.
std::vector<int> convexHull(const std::vector<Vec2>& points);
bool pointInPolygon(Vec2 p, const std::vector<Vec2>& polygon);

// Setup-pose vertex positions in slot-bone space.
std::vector<Vec2> setupLocalVertices(const Skeleton& sk, const Slot& slot, const Attachment& att);
// Reorders vertices so the outline comes first and sets hullLength (derived from boundary
// edges). False if the mesh boundary is not a single loop.
bool ensureHull(Attachment& att);
// Triangulates again from the current setup-pose vertex positions (needs hullLength >= 3).
bool retriangulate(const Skeleton& sk, const Slot& slot, Attachment& att);
// Adds a vertex at a setup-pose slot-bone position. `hullAfter` >= 0 inserts it into the
// outline after that hull vertex, otherwise it is an interior point. Weighted meshes get
// weights interpolated from their surroundings; uvs come from `img` (null: polygons).
// Returns the new vertex index or -1.
int addMeshVertex(const Skeleton& sk, const Slot& slot, Attachment& att, const Image* img, Vec2 setupLocal,
                  int hullAfter);
// Removes vertices (the outline keeps at least 3). False if nothing was removed.
bool removeMeshVertices(const Skeleton& sk, const Slot& slot, Attachment& att, std::vector<int> vertices);
// Moves a vertex like setVertexSetupWorld and, for meshes, makes its uv follow (edit mode).
void setVertexUV(Attachment& att, int vertex, Vec2 setupLocal, const Affine& imageToLocalMap, const Image& img);
// Polygon (clipping / bounding box) as a rectangle in slot-bone space.
void makePolygon(Attachment& att, const std::vector<Vec2>& points);

// ---------------------------------------------------------------- weights

// Bind a mesh to `bones` with automatic distance-based weights (setup pose).
void bindMesh(const Skeleton& sk, const Slot& slot, Attachment& att, const std::vector<int>& bones,
              int maxInfluences = 4);
// Back to an unweighted mesh, frozen at its current setup-pose shape.
void unbindMesh(const Skeleton& sk, const Slot& slot, Attachment& att);

float vertexWeight(const Attachment& att, int vertex, int bone);
// Add `delta` to a vertex's weight for `bone`, rescaling the other unlocked influences so the
// total stays 1. Weights of `locked` bones never change.
void paintWeight(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex, int bone, float delta,
                 const std::vector<int>& locked = {});
// Set a vertex's weight for `bone` to `value` (same rules as paintWeight).
void setWeight(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex, int bone, float value,
               const std::vector<int>& locked = {});
// Blend each vertex's weights towards the average of its neighbours (empty = all vertices).
void smoothWeights(const Skeleton& sk, const Slot& slot, Attachment& att, const std::vector<int>& vertices,
                   float amount, const std::vector<int>& locked = {});
// Drop influences below `threshold` and keep at most `maxInfluences` per vertex. Returns removed count.
int pruneWeights(Attachment& att, float threshold, int maxInfluences);
// Bones that influence any vertex of the mesh.
std::vector<int> meshBones(const Attachment& att);

// Move one mesh vertex to a setup-pose world position.
void setVertexSetupWorld(const Skeleton& sk, const Pose& setup, const Slot& slot, Attachment& att,
                         int vertex, Vec2 world);

} // namespace rig
