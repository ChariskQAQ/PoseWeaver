#pragma once
#include "core/pose.h"

namespace rig {

// Turn a region attachment into a cols x rows grid mesh covering the image. When `rgba`
// (width*height*4 bytes) is given, cells whose pixels are all at or below
// `alphaThreshold` are dropped. Returns false if nothing remains.
bool convertRegionToMesh(Attachment& att, const Image& img, const unsigned char* rgba,
                         int cols, int rows, unsigned char alphaThreshold = 8);

// Bind a mesh to `bones` with automatic distance-based weights (setup pose).
void bindMesh(const Skeleton& sk, const Slot& slot, Attachment& att, const std::vector<int>& bones,
              int maxInfluences = 4);
// Back to an unweighted mesh, frozen at its current setup-pose shape.
void unbindMesh(const Skeleton& sk, const Slot& slot, Attachment& att);

float vertexWeight(const Attachment& att, int vertex, int bone);
// Add `delta` to a vertex's weight for `bone`, rescaling the other influences to sum to 1.
void paintWeight(const Skeleton& sk, const Slot& slot, Attachment& att, int vertex, int bone, float delta);
// Bones that influence any vertex of the mesh.
std::vector<int> meshBones(const Attachment& att);

// Move one mesh vertex to a setup-pose world position.
void setVertexSetupWorld(const Skeleton& sk, const Pose& setup, const Slot& slot, Attachment& att,
                         int vertex, Vec2 world);

} // namespace rig
