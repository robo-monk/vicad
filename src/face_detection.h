#pragma once

#include <cstddef>
#include <vector>

#include "manifold/manifold.h"

namespace vicad {

enum class FacePrimitiveType {
    Unknown,
    Plane,
    Sphere,
    Cylinder,
};

struct FaceDetectionResult {
    std::vector<int> triRegion;
    std::vector<std::vector<uint32_t>> regions;
    std::vector<FacePrimitiveType> regionType;
};

FaceDetectionResult DetectMeshFaces(const manifold::MeshGL &mesh, float maxDihedralDegrees);

int PickFaceRegionByRay(const manifold::MeshGL &mesh,
                        const FaceDetectionResult &faces,
                        double rayOriginX, double rayOriginY, double rayOriginZ,
                        double rayDirX, double rayDirY, double rayDirZ,
                        double *outDistance);

const char *FacePrimitiveTypeName(FacePrimitiveType type);

}  // namespace vicad
