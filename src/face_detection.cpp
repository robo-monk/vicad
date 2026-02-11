#include "face_detection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>

namespace vicad {

namespace {

struct Vec3d {
    double x;
    double y;
    double z;
};

struct RegionFit {
    FacePrimitiveType type = FacePrimitiveType::Unknown;
    Vec3d planeN = {0.0, 0.0, 0.0};
    double planeD = 0.0;
    double planeRms = std::numeric_limits<double>::infinity();
    Vec3d sphereC = {0.0, 0.0, 0.0};
    double sphereR = 0.0;
    double sphereRms = std::numeric_limits<double>::infinity();
    Vec3d cylinderAxis = {0.0, 0.0, 0.0};
    Vec3d cylinderPoint = {0.0, 0.0, 0.0};
    double cylinderR = 0.0;
    double cylinderRms = std::numeric_limits<double>::infinity();
};

struct DisjointSet {
    std::vector<int> p;
    std::vector<int> r;

    explicit DisjointSet(size_t n) : p(n), r(n, 0) {
        std::iota(p.begin(), p.end(), 0);
    }

    int find(int x) {
        if (p[x] == x) return x;
        p[x] = find(p[x]);
        return p[x];
    }

    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (r[a] < r[b]) std::swap(a, b);
        p[b] = a;
        if (r[a] == r[b]) ++r[a];
    }
};

static Vec3d add(const Vec3d &a, const Vec3d &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3d sub(const Vec3d &a, const Vec3d &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3d mul(const Vec3d &v, double s) {
    return {v.x * s, v.y * s, v.z * s};
}

static Vec3d cross(const Vec3d &a, const Vec3d &b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static double dot(const Vec3d &a, const Vec3d &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double length(const Vec3d &v) {
    return std::sqrt(dot(v, v));
}

static Vec3d normalize(const Vec3d &v) {
    const double l = length(v);
    if (l <= 1e-30) return {0.0, 0.0, 0.0};
    return {v.x / l, v.y / l, v.z / l};
}

static Vec3d mesh_pos(const manifold::MeshGL &mesh, uint32_t idx) {
    const size_t base = (size_t)idx * (size_t)mesh.numProp;
    return {
        (double)mesh.vertProperties[base + 0],
        (double)mesh.vertProperties[base + 1],
        (double)mesh.vertProperties[base + 2],
    };
}

static uint64_t edge_key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (uint64_t)a << 32 | (uint64_t)b;
}

static bool solve_4x4(double m[4][5], double out[4]) {
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        double best = std::fabs(m[col][col]);
        for (int row = col + 1; row < 4; ++row) {
            const double v = std::fabs(m[row][col]);
            if (v > best) {
                best = v;
                pivot = row;
            }
        }
        if (best < 1e-14) return false;
        if (pivot != col) std::swap(m[pivot], m[col]);

        const double inv = 1.0 / m[col][col];
        for (int k = col; k < 5; ++k) m[col][k] *= inv;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            const double f = m[row][col];
            if (std::fabs(f) < 1e-16) continue;
            for (int k = col; k < 5; ++k) m[row][k] -= f * m[col][k];
        }
    }
    for (int i = 0; i < 4; ++i) out[i] = m[i][4];
    return true;
}

static bool solve_3x3(double m[3][4], double out[3]) {
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        double best = std::fabs(m[col][col]);
        for (int row = col + 1; row < 3; ++row) {
            const double v = std::fabs(m[row][col]);
            if (v > best) {
                best = v;
                pivot = row;
            }
        }
        if (best < 1e-14) return false;
        if (pivot != col) std::swap(m[pivot], m[col]);

        const double inv = 1.0 / m[col][col];
        for (int k = col; k < 4; ++k) m[col][k] *= inv;
        for (int row = 0; row < 3; ++row) {
            if (row == col) continue;
            const double f = m[row][col];
            if (std::fabs(f) < 1e-16) continue;
            for (int k = col; k < 4; ++k) m[row][k] -= f * m[col][k];
        }
    }
    for (int i = 0; i < 3; ++i) out[i] = m[i][3];
    return true;
}

static bool ray_intersect_triangle(const Vec3d &orig, const Vec3d &dir,
                                   const Vec3d &v0, const Vec3d &v1, const Vec3d &v2,
                                   double *out_t) {
    const Vec3d e1 = sub(v1, v0);
    const Vec3d e2 = sub(v2, v0);
    const Vec3d p = cross(dir, e2);
    const double det = dot(e1, p);
    if (std::fabs(det) < 1e-12) return false;
    const double inv_det = 1.0 / det;
    const Vec3d tvec = sub(orig, v0);
    const double u = dot(tvec, p) * inv_det;
    if (u < 0.0 || u > 1.0) return false;
    const Vec3d q = cross(tvec, e1);
    const double v = dot(dir, q) * inv_det;
    if (v < 0.0 || u + v > 1.0) return false;
    const double t = dot(e2, q) * inv_det;
    if (t <= 1e-9) return false;
    if (out_t) *out_t = t;
    return true;
}

static RegionFit classify_region(const std::vector<uint32_t> &tris,
                                 const std::vector<Vec3d> &triCenters,
                                 const std::vector<Vec3d> &triNormals,
                                 double planeTol,
                                 double sphereTol,
                                 double cylinderTol) {
    RegionFit fit = {};
    if (tris.empty()) return fit;

    Vec3d centroid = {0.0, 0.0, 0.0};
    Vec3d nsum = {0.0, 0.0, 0.0};
    for (uint32_t t : tris) {
        centroid = add(centroid, triCenters[t]);
        nsum = add(nsum, triNormals[t]);
    }
    centroid = mul(centroid, 1.0 / (double)tris.size());
    fit.planeN = normalize(nsum);
    fit.planeD = -dot(fit.planeN, centroid);

    double planeErr2 = 0.0;
    for (uint32_t t : tris) {
        const double dist = dot(fit.planeN, triCenters[t]) + fit.planeD;
        planeErr2 += dist * dist;
    }
    fit.planeRms = std::sqrt(planeErr2 / (double)tris.size());

    if (tris.size() >= 6) {
        double ata[4][4] = {};
        double atb[4] = {};
        for (uint32_t t : tris) {
            const Vec3d p = triCenters[t];
            const Vec3d n = triNormals[t];
            const double rows[3][5] = {
                {1.0, 0.0, 0.0, n.x, p.x},
                {0.0, 1.0, 0.0, n.y, p.y},
                {0.0, 0.0, 1.0, n.z, p.z},
            };
            for (int r = 0; r < 3; ++r) {
                for (int i = 0; i < 4; ++i) {
                    atb[i] += rows[r][i] * rows[r][4];
                    for (int j = 0; j < 4; ++j) {
                        ata[i][j] += rows[r][i] * rows[r][j];
                    }
                }
            }
        }

        double aug[4][5] = {};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) aug[i][j] = ata[i][j];
            aug[i][4] = atb[i];
        }
        double x[4] = {};
        if (solve_4x4(aug, x) && std::isfinite(x[3]) && x[3] > 1e-9) {
            fit.sphereC = {x[0], x[1], x[2]};
            fit.sphereR = x[3];
            double sphereErr2 = 0.0;
            for (uint32_t t : tris) {
                const Vec3d p = triCenters[t];
                const Vec3d n = triNormals[t];
                const Vec3d est = add(fit.sphereC, mul(n, fit.sphereR));
                const Vec3d d = sub(est, p);
                sphereErr2 += dot(d, d);
            }
            fit.sphereRms = std::sqrt(sphereErr2 / (double)tris.size());
        }
    }

    if (tris.size() >= 8) {
        Vec3d axis = {0.0, 0.0, 0.0};
        for (size_t i = 1; i < tris.size(); ++i) {
            Vec3d c = cross(triNormals[tris[i - 1]], triNormals[tris[i]]);
            if (length(c) < 1e-8) continue;
            if (dot(axis, c) < 0.0) c = mul(c, -1.0);
            axis = add(axis, c);
        }
        axis = normalize(axis);
        if (length(axis) > 1e-8) {
            Vec3d helper = std::fabs(axis.z) < 0.9 ? Vec3d{0.0, 0.0, 1.0} : Vec3d{1.0, 0.0, 0.0};
            Vec3d u = normalize(cross(axis, helper));
            Vec3d v = cross(axis, u);

            double ata[3][3] = {};
            double atb[3] = {};
            for (uint32_t t : tris) {
                const Vec3d p = triCenters[t];
                const double x = dot(p, u);
                const double y = dot(p, v);
                const double row[3] = {x, y, 1.0};
                const double rhs = -(x * x + y * y);
                for (int i = 0; i < 3; ++i) {
                    atb[i] += row[i] * rhs;
                    for (int j = 0; j < 3; ++j) {
                        ata[i][j] += row[i] * row[j];
                    }
                }
            }

            double aug[3][4] = {};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) aug[i][j] = ata[i][j];
                aug[i][3] = atb[i];
            }
            double x[3] = {};
            if (solve_3x3(aug, x)) {
                const double cx = -0.5 * x[0];
                const double cy = -0.5 * x[1];
                const double rr = cx * cx + cy * cy - x[2];
                if (std::isfinite(rr) && rr > 1e-12) {
                    const double r = std::sqrt(rr);
                    const Vec3d c3 = add(mul(u, cx), mul(v, cy));

                    double radErr2 = 0.0;
                    double ndotErr2 = 0.0;
                    for (uint32_t t : tris) {
                        const Vec3d p = triCenters[t];
                        const Vec3d d = sub(p, c3);
                        const double ax = dot(d, axis);
                        const Vec3d radial = sub(d, mul(axis, ax));
                        const double rho = length(radial);
                        const double re = rho - r;
                        radErr2 += re * re;
                        const double na = dot(triNormals[t], axis);
                        ndotErr2 += na * na;
                    }
                    const double radialRms = std::sqrt(radErr2 / (double)tris.size());
                    const double normalRms = std::sqrt(ndotErr2 / (double)tris.size());
                    fit.cylinderAxis = axis;
                    fit.cylinderPoint = c3;
                    fit.cylinderR = r;
                    fit.cylinderRms = std::sqrt(radialRms * radialRms + (normalRms * r) * (normalRms * r));
                }
            }
        }
    }

    const bool planeOk = fit.planeRms <= planeTol;
    const bool sphereOk = fit.sphereRms <= sphereTol;
    const bool cylOk = fit.cylinderRms <= cylinderTol;
    const double pn = fit.planeRms / (planeTol > 1e-12 ? planeTol : 1.0);
    const double sn = fit.sphereRms / (sphereTol > 1e-12 ? sphereTol : 1.0);
    const double cn = fit.cylinderRms / (cylinderTol > 1e-12 ? cylinderTol : 1.0);
    if (planeOk || sphereOk || cylOk) {
        fit.type = FacePrimitiveType::Unknown;
        double best = std::numeric_limits<double>::infinity();
        if (planeOk && pn < best) {
            best = pn;
            fit.type = FacePrimitiveType::Plane;
        }
        if (sphereOk && sn < best) {
            best = sn;
            fit.type = FacePrimitiveType::Sphere;
        }
        if (cylOk && cn < best) {
            fit.type = FacePrimitiveType::Cylinder;
        }
    } else {
        fit.type = FacePrimitiveType::Unknown;
    }
    return fit;
}

static bool compatible_for_merge(const RegionFit &a, const RegionFit &b,
                                 double planeTol, double sphereTol, double cylinderTol) {
    constexpr double kPi = 3.14159265358979323846;
    const double planeDotTol = std::cos(8.0 * kPi / 180.0);
    if (a.type == FacePrimitiveType::Plane && b.type == FacePrimitiveType::Plane) {
        Vec3d an = a.planeN;
        double ad = a.planeD;
        Vec3d bn = b.planeN;
        double bd = b.planeD;
        if (dot(an, bn) < 0.0) {
            bn = mul(bn, -1.0);
            bd = -bd;
        }
        if (dot(an, bn) < planeDotTol) return false;
        return std::fabs(ad - bd) <= planeTol * 1.5;
    }
    if (a.type == FacePrimitiveType::Sphere && b.type == FacePrimitiveType::Sphere) {
        const double cdist = length(sub(a.sphereC, b.sphereC));
        const double rdiff = std::fabs(a.sphereR - b.sphereR);
        return cdist <= sphereTol * 2.0 && rdiff <= sphereTol * 2.0;
    }
    if (a.type == FacePrimitiveType::Cylinder && b.type == FacePrimitiveType::Cylinder) {
        Vec3d aa = a.cylinderAxis;
        Vec3d ba = b.cylinderAxis;
        if (dot(aa, ba) < 0.0) ba = mul(ba, -1.0);
        if (dot(aa, ba) < planeDotTol) return false;
        const double rdiff = std::fabs(a.cylinderR - b.cylinderR);
        if (rdiff > cylinderTol * 2.0) return false;
        const Vec3d cdelta = sub(b.cylinderPoint, a.cylinderPoint);
        const Vec3d cross_ax = cross(cdelta, aa);
        const double axisDist = length(cross_ax);
        return axisDist <= cylinderTol * 2.5;
    }
    return false;
}

}  // namespace

FaceDetectionResult DetectMeshFaces(const manifold::MeshGL &mesh, float maxDihedralDegrees) {
    FaceDetectionResult out = {};
    const uint32_t triCount = (uint32_t)mesh.NumTri();
    if (triCount == 0 || mesh.numProp < 3) return out;

    out.triRegion.assign(triCount, -1);

    std::vector<Vec3d> triNormal(triCount);
    std::vector<Vec3d> triCenter(triCount);
    triNormal.reserve(triCount);
    triCenter.reserve(triCount);
    Vec3d mn = mesh_pos(mesh, mesh.triVerts[0]);
    Vec3d mx = mn;

    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];
        const Vec3d p0 = mesh_pos(mesh, i0);
        const Vec3d p1 = mesh_pos(mesh, i1);
        const Vec3d p2 = mesh_pos(mesh, i2);
        triNormal[tri] = normalize(cross(sub(p1, p0), sub(p2, p0)));
        triCenter[tri] = mul(add(add(p0, p1), p2), 1.0 / 3.0);

        mn.x = std::min({mn.x, p0.x, p1.x, p2.x});
        mn.y = std::min({mn.y, p0.y, p1.y, p2.y});
        mn.z = std::min({mn.z, p0.z, p1.z, p2.z});
        mx.x = std::max({mx.x, p0.x, p1.x, p2.x});
        mx.y = std::max({mx.y, p0.y, p1.y, p2.y});
        mx.z = std::max({mx.z, p0.z, p1.z, p2.z});
    }

    const double bboxDiag = std::max(length(sub(mx, mn)), 1e-6);
    const double planeTol = std::max(1e-5, bboxDiag * 0.003);
    const double sphereTol = std::max(1e-5, bboxDiag * 0.005);
    const double cylinderTol = std::max(1e-5, bboxDiag * 0.0055);

    std::vector<std::vector<uint32_t>> neighbors(triCount);
    std::unordered_map<uint64_t, std::vector<uint32_t>> edgeToTris;
    edgeToTris.reserve((size_t)triCount * 2);
    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];
        edgeToTris[edge_key(i0, i1)].push_back(tri);
        edgeToTris[edge_key(i1, i2)].push_back(tri);
        edgeToTris[edge_key(i2, i0)].push_back(tri);
    }
    for (const auto &it : edgeToTris) {
        const std::vector<uint32_t> &tris = it.second;
        for (size_t i = 0; i < tris.size(); ++i) {
            for (size_t j = i + 1; j < tris.size(); ++j) {
                neighbors[tris[i]].push_back(tris[j]);
                neighbors[tris[j]].push_back(tris[i]);
            }
        }
    }
    for (std::vector<uint32_t> &adj : neighbors) {
        std::sort(adj.begin(), adj.end());
        adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
    }

    constexpr double kPi = 3.14159265358979323846;
    const double threshold = std::cos((double)maxDihedralDegrees * kPi / 180.0);

    std::queue<uint32_t> q;
    for (uint32_t seed = 0; seed < triCount; ++seed) {
        if (out.triRegion[seed] != -1) continue;
        const int regionId = (int)out.regions.size();
        out.regions.push_back({});

        out.triRegion[seed] = regionId;
        q.push(seed);
        while (!q.empty()) {
            const uint32_t tri = q.front();
            q.pop();
            out.regions.back().push_back(tri);

            for (const uint32_t nb : neighbors[tri]) {
                if (out.triRegion[nb] != -1) continue;
                const double d = dot(triNormal[tri], triNormal[nb]);
                if (d < threshold) continue;
                out.triRegion[nb] = regionId;
                q.push(nb);
            }
        }
    }

    std::vector<std::pair<int, int>> regionAdj;
    regionAdj.reserve((size_t)triCount * 2);
    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const int a = out.triRegion[tri];
        for (uint32_t nb : neighbors[tri]) {
            const int b = out.triRegion[nb];
            if (a == b) continue;
            const int lo = std::min(a, b);
            const int hi = std::max(a, b);
            regionAdj.push_back({lo, hi});
        }
    }
    std::sort(regionAdj.begin(), regionAdj.end());
    regionAdj.erase(std::unique(regionAdj.begin(), regionAdj.end()), regionAdj.end());

    std::vector<RegionFit> fits(out.regions.size());
    for (size_t i = 0; i < out.regions.size(); ++i) {
        fits[i] = classify_region(out.regions[i], triCenter, triNormal, planeTol, sphereTol, cylinderTol);
    }

    DisjointSet dsu(out.regions.size());
    for (const auto &e : regionAdj) {
        if (compatible_for_merge(fits[(size_t)e.first], fits[(size_t)e.second], planeTol, sphereTol, cylinderTol)) {
            dsu.unite(e.first, e.second);
        }
    }

    std::unordered_map<int, int> rootToNew;
    std::vector<std::vector<uint32_t>> merged;
    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const int old = out.triRegion[tri];
        const int root = dsu.find(old);
        auto it = rootToNew.find(root);
        int id = -1;
        if (it == rootToNew.end()) {
            id = (int)merged.size();
            rootToNew[root] = id;
            merged.push_back({});
        } else {
            id = it->second;
        }
        out.triRegion[tri] = id;
        merged[(size_t)id].push_back(tri);
    }
    out.regions = std::move(merged);

    out.regionType.resize(out.regions.size(), FacePrimitiveType::Unknown);
    for (size_t i = 0; i < out.regions.size(); ++i) {
        const RegionFit fit = classify_region(out.regions[i], triCenter, triNormal, planeTol, sphereTol, cylinderTol);
        out.regionType[i] = fit.type;
    }

    return out;
}

int PickFaceRegionByRay(const manifold::MeshGL &mesh,
                        const FaceDetectionResult &faces,
                        double rayOriginX, double rayOriginY, double rayOriginZ,
                        double rayDirX, double rayDirY, double rayDirZ,
                        double *outDistance) {
    const uint32_t triCount = (uint32_t)mesh.NumTri();
    if (triCount == 0 || faces.triRegion.size() != triCount) return -1;

    const Vec3d orig = {rayOriginX, rayOriginY, rayOriginZ};
    const Vec3d dir = normalize({rayDirX, rayDirY, rayDirZ});

    double bestT = std::numeric_limits<double>::infinity();
    int bestRegion = -1;
    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];
        const Vec3d p0 = mesh_pos(mesh, i0);
        const Vec3d p1 = mesh_pos(mesh, i1);
        const Vec3d p2 = mesh_pos(mesh, i2);
        double t = 0.0;
        if (!ray_intersect_triangle(orig, dir, p0, p1, p2, &t)) continue;
        if (t >= bestT) continue;
        bestT = t;
        bestRegion = faces.triRegion[tri];
    }

    if (bestRegion >= 0 && outDistance) *outDistance = bestT;
    return bestRegion;
}

const char *FacePrimitiveTypeName(FacePrimitiveType type) {
    switch (type) {
        case FacePrimitiveType::Plane: return "Plane";
        case FacePrimitiveType::Sphere: return "Sphere";
        case FacePrimitiveType::Cylinder: return "Cylinder";
        default: return "Unknown";
    }
}

}  // namespace vicad
