#pragma once

#include <cstdint>
#include <vector>

#include "manifold/manifold.h"

namespace vicad {

enum EdgeClassFlags : uint8_t {
    EdgeClassNone = 0,
    EdgeClassBoundary = 1 << 0,
    EdgeClassSharp = 1 << 1,
    EdgeClassNonManifold = 1 << 2,
};

struct EdgeVec3 {
    double x;
    double y;
    double z;
};

struct EdgeRecord {
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    int triA = -1;
    int triB = -1;
    EdgeVec3 nA = {0.0, 0.0, 0.0};
    EdgeVec3 nB = {0.0, 0.0, 0.0};
};

struct EdgeDetectionResult {
    std::vector<EdgeRecord> edges;
    std::vector<uint8_t> edgeFlags;
    std::vector<int> sharpEdgeIndices;
    std::vector<int> boundaryEdgeIndices;
    std::vector<int> nonManifoldEdgeIndices;
    std::vector<std::vector<int>> featureChains;
    std::vector<int> edgeFeatureChain;
};

struct SilhouetteResult {
    std::vector<int> silhouetteEdgeIndices;
    std::vector<uint8_t> isSilhouette;
};

EdgeDetectionResult BuildEdgeTopology(const manifold::MeshGL &mesh, float sharpAngleDeg);

SilhouetteResult ComputeSilhouetteEdges(const manifold::MeshGL &mesh,
                                        const EdgeDetectionResult &edges,
                                        double eyeX, double eyeY, double eyeZ);

int PickEdgeByRay(const manifold::MeshGL &mesh,
                  const EdgeDetectionResult &edges,
                  const SilhouetteResult &silhouette,
                  double rayOriginX, double rayOriginY, double rayOriginZ,
                  double rayDirX, double rayDirY, double rayDirZ,
                  double pickRadius,
                  double *outDistance);

}  // namespace vicad
