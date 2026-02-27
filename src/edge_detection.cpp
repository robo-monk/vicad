#include "edge_detection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace vicad {

namespace {

struct Vec3d {
    double x;
    double y;
    double z;
};

struct EdgeAccum {
    uint32_t renderV0 = 0;
    uint32_t renderV1 = 0;
    bool hasRenderVerts = false;
    std::vector<int> tris;
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
    if (l <= 1e-20) return {0.0, 0.0, 0.0};
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

static double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static uint64_t edge_key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | (uint64_t)b;
}

static bool point_ray_distance(const Vec3d &p,
                               const Vec3d &rayOrig,
                               const Vec3d &rayDir,
                               double *outT,
                               double *outDist) {
    const Vec3d op = sub(p, rayOrig);
    const double t = dot(op, rayDir);
    if (!std::isfinite(t) || t <= 1e-9) return false;
    const Vec3d q = add(rayOrig, mul(rayDir, t));
    const double d = length(sub(p, q));
    if (!std::isfinite(d)) return false;
    if (outT) *outT = t;
    if (outDist) *outDist = d;
    return true;
}

static bool segment_ray_distance(const Vec3d &a,
                                 const Vec3d &b,
                                 const Vec3d &rayOrig,
                                 const Vec3d &rayDir,
                                 double *outT,
                                 double *outDist) {
    const Vec3d seg = sub(b, a);
    const double seg_len2 = dot(seg, seg);
    if (!std::isfinite(seg_len2)) return false;
    if (seg_len2 <= 1e-20) {
        return point_ray_distance(a, rayOrig, rayDir, outT, outDist);
    }

    bool have = false;
    double best_t = std::numeric_limits<double>::infinity();
    double best_d = std::numeric_limits<double>::infinity();

    auto consider = [&](double t, double u) {
        if (!std::isfinite(t) || !std::isfinite(u)) return;
        if (t < 0.0) return;
        if (u < 0.0 || u > 1.0) return;
        const Vec3d pr = add(rayOrig, mul(rayDir, t));
        const Vec3d ps = add(a, mul(seg, u));
        const double d = length(sub(pr, ps));
        if (!std::isfinite(d)) return;
        if (!have ||
            d < best_d - 1e-9 ||
            (std::fabs(d - best_d) <= 1e-9 && t < best_t)) {
            have = true;
            best_t = t;
            best_d = d;
        }
    };

    // Unconstrained closest points between infinite line(ray) and line(segment),
    // then accept only if it lands on ray and segment interior.
    const double b_dot = dot(rayDir, seg);
    const Vec3d w0 = sub(rayOrig, a);
    const double d_dot = dot(rayDir, w0);
    const double e_dot = dot(seg, w0);
    const double denom = seg_len2 - b_dot * b_dot;
    if (std::fabs(denom) > 1e-12) {
        const double t = (b_dot * e_dot - seg_len2 * d_dot) / denom;
        const double u = (e_dot - b_dot * d_dot) / denom;
        consider(t, u);
    }

    double t0 = 0.0;
    double d0 = 0.0;
    if (point_ray_distance(a, rayOrig, rayDir, &t0, &d0)) {
        consider(t0, 0.0);
    }
    double t1 = 0.0;
    double d1 = 0.0;
    if (point_ray_distance(b, rayOrig, rayDir, &t1, &d1)) {
        consider(t1, 1.0);
    }

    // Ray origin projected onto segment (t = 0) handles the boundary case where
    // the closest ray point lies before the ray start.
    const double u_origin = clamp01(dot(sub(rayOrig, a), seg) / seg_len2);
    consider(0.0, u_origin);

    if (!have) return false;
    if (outT) *outT = best_t;
    if (outDist) *outDist = best_d;
    return true;
}

struct DisjointSet {
    std::vector<uint32_t> parent;
    std::vector<uint8_t> rank;

    explicit DisjointSet(size_t n) : parent(n), rank(n, 0) {
        std::iota(parent.begin(), parent.end(), 0);
    }

    uint32_t find(uint32_t x) {
        while (parent[(size_t)x] != x) {
            parent[(size_t)x] = parent[(size_t)parent[(size_t)x]];
            x = parent[(size_t)x];
        }
        return x;
    }

    void unite(uint32_t a, uint32_t b) {
        uint32_t ra = find(a);
        uint32_t rb = find(b);
        if (ra == rb) return;
        const uint8_t rankA = rank[(size_t)ra];
        const uint8_t rankB = rank[(size_t)rb];
        if (rankA < rankB) {
            parent[(size_t)ra] = rb;
            return;
        }
        if (rankA > rankB) {
            parent[(size_t)rb] = ra;
            return;
        }
        parent[(size_t)rb] = ra;
        rank[(size_t)ra] += 1;
    }
};

static void add_edge(std::unordered_map<uint64_t, EdgeAccum> &edgeMap,
                     DisjointSet *dsu,
                     uint32_t v0,
                     uint32_t v1,
                     int tri) {
    const uint32_t c0 = dsu->find(v0);
    const uint32_t c1 = dsu->find(v1);
    if (c0 == c1) return;
    const uint64_t key = edge_key(c0, c1);
    EdgeAccum &acc = edgeMap[key];
    if (!acc.hasRenderVerts) {
        acc.renderV0 = v0;
        acc.renderV1 = v1;
        acc.hasRenderVerts = true;
    }
    acc.tris.push_back(tri);
}

}  // namespace

EdgeDetectionResult BuildEdgeTopology(const manifold::MeshGL &mesh) {
    EdgeDetectionResult out = {};
    const uint32_t triCount = (uint32_t)mesh.NumTri();
    if (triCount == 0 || mesh.numProp < 3) return out;
    if (mesh.faceID.size() != triCount) return out;

    std::vector<Vec3d> triNormal(triCount);
    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];
        const Vec3d p0 = mesh_pos(mesh, i0);
        const Vec3d p1 = mesh_pos(mesh, i1);
        const Vec3d p2 = mesh_pos(mesh, i2);
        triNormal[tri] = normalize(cross(sub(p1, p0), sub(p2, p0)));
    }

    const uint32_t numVerts = (uint32_t)mesh.NumVert();
    DisjointSet dsu(numVerts);
    const size_t mergeCount = std::min(mesh.mergeFromVert.size(), mesh.mergeToVert.size());
    for (size_t i = 0; i < mergeCount; ++i) {
        const uint32_t from = mesh.mergeFromVert[i];
        const uint32_t to = mesh.mergeToVert[i];
        if (from >= numVerts || to >= numVerts) continue;
        dsu.unite(from, to);
    }

    std::vector<int> triRunSlot(triCount, 0);
    if (mesh.runIndex.size() >= 2) {
        for (size_t run = 0; run + 1 < mesh.runIndex.size(); ++run) {
            const uint32_t begin = mesh.runIndex[run] / 3;
            const uint32_t end = mesh.runIndex[run + 1] / 3;
            if (begin >= triCount || end <= begin) continue;
            const uint32_t triEnd = std::min(end, triCount);
            for (uint32_t tri = begin; tri < triEnd; ++tri) {
                triRunSlot[(size_t)tri] = (int)run;
            }
        }
    }

    std::unordered_map<uint64_t, EdgeAccum> edgeMap;
    edgeMap.reserve((size_t)triCount * 2);
    for (uint32_t tri = 0; tri < triCount; ++tri) {
        const uint32_t i0 = mesh.triVerts[tri * 3 + 0];
        const uint32_t i1 = mesh.triVerts[tri * 3 + 1];
        const uint32_t i2 = mesh.triVerts[tri * 3 + 2];
        add_edge(edgeMap, &dsu, i0, i1, (int)tri);
        add_edge(edgeMap, &dsu, i1, i2, (int)tri);
        add_edge(edgeMap, &dsu, i2, i0, (int)tri);
    }

    std::vector<uint64_t> sortedKeys;
    sortedKeys.reserve(edgeMap.size());
    for (const auto &it : edgeMap) {
        sortedKeys.push_back(it.first);
    }
    std::sort(sortedKeys.begin(), sortedKeys.end());

    out.edges.reserve(sortedKeys.size());
    out.edgeFlags.reserve(sortedKeys.size());
    out.featureEdgeIndices.clear();
    out.nonManifoldEdgeIndices.clear();

    for (const uint64_t key : sortedKeys) {
        const auto found = edgeMap.find(key);
        if (found == edgeMap.end()) continue;
        const EdgeAccum &acc = found->second;
        if (!acc.hasRenderVerts) continue;

        std::vector<int> tris = acc.tris;
        std::sort(tris.begin(), tris.end());
        tris.erase(std::unique(tris.begin(), tris.end()), tris.end());

        uint8_t flags = EdgeClassNone;
        bool keep = false;
        if (tris.size() == 1) {
            flags |= EdgeClassFeature;
            keep = true;
        } else if (tris.size() == 2) {
            const int triA = tris[0];
            const int triB = tris[1];
            if (triRunSlot[(size_t)triA] != triRunSlot[(size_t)triB] ||
                mesh.faceID[(size_t)triA] != mesh.faceID[(size_t)triB]) {
                flags |= EdgeClassFeature;
                keep = true;
            }
        } else if (tris.size() > 2) {
            flags |= EdgeClassNonManifold;
            keep = true;
        }
        if (!keep) continue;

        EdgeRecord rec = {};
        rec.v0 = acc.renderV0;
        rec.v1 = acc.renderV1;
        if (!tris.empty()) {
            rec.triA = tris[0];
            rec.nA = {triNormal[(size_t)tris[0]].x,
                      triNormal[(size_t)tris[0]].y,
                      triNormal[(size_t)tris[0]].z};
        }
        if (tris.size() >= 2) {
            rec.triB = tris[1];
            rec.nB = {triNormal[(size_t)tris[1]].x,
                      triNormal[(size_t)tris[1]].y,
                      triNormal[(size_t)tris[1]].z};
        }

        out.edges.push_back(rec);
        out.edgeFlags.push_back(flags);
        const int idx = (int)out.edges.size() - 1;
        if ((flags & EdgeClassFeature) != 0) out.featureEdgeIndices.push_back(idx);
        if ((flags & EdgeClassNonManifold) != 0) out.nonManifoldEdgeIndices.push_back(idx);
    }

    return out;
}

SilhouetteResult ComputeSilhouetteEdges(const manifold::MeshGL &mesh,
                                        const EdgeDetectionResult &edges,
                                        double eyeX, double eyeY, double eyeZ) {
    SilhouetteResult out = {};
    out.isSilhouette.assign(edges.edges.size(), 0);
    if (mesh.numProp < 3 || edges.edges.empty()) return out;

    const Vec3d eye = {eyeX, eyeY, eyeZ};

    for (const int idx : edges.featureEdgeIndices) {
        if (idx < 0 || (size_t)idx >= edges.edges.size()) continue;
        const EdgeRecord &e = edges.edges[(size_t)idx];
        if (e.triA < 0 || e.triB < 0) continue;

        const Vec3d p0 = mesh_pos(mesh, e.v0);
        const Vec3d p1 = mesh_pos(mesh, e.v1);
        const Vec3d mid = mul(add(p0, p1), 0.5);
        const Vec3d viewDir = normalize(sub(eye, mid));

        const Vec3d nA = {e.nA.x, e.nA.y, e.nA.z};
        const Vec3d nB = {e.nB.x, e.nB.y, e.nB.z};
        const double da = dot(nA, viewDir);
        const double db = dot(nB, viewDir);
        if (!std::isfinite(da) || !std::isfinite(db)) continue;

        if ((da > 0.0 && db <= 0.0) || (da <= 0.0 && db > 0.0)) {
            out.isSilhouette[(size_t)idx] = 1;
            out.silhouetteEdgeIndices.push_back(idx);
        }
    }

    return out;
}

int PickEdgeByRay(const manifold::MeshGL &mesh,
                  const EdgeDetectionResult &edges,
                  const SilhouetteResult & /*silhouette*/,
                  double rayOriginX, double rayOriginY, double rayOriginZ,
                  double rayDirX, double rayDirY, double rayDirZ,
                  double pickRadius,
                  double *outDistance) {
    if (mesh.numProp < 3 || edges.edges.empty()) return -1;

    const Vec3d rayOrig = {rayOriginX, rayOriginY, rayOriginZ};
    const Vec3d rayDir = normalize({rayDirX, rayDirY, rayDirZ});
    if (length(rayDir) <= 1e-20) return -1;

    std::vector<uint8_t> candidate(edges.edges.size(), 0);
    for (int idx : edges.featureEdgeIndices) {
        if (idx >= 0 && (size_t)idx < candidate.size()) candidate[(size_t)idx] = 1;
    }
    for (int idx : edges.nonManifoldEdgeIndices) {
        if (idx >= 0 && (size_t)idx < candidate.size()) candidate[(size_t)idx] = 1;
    }

    double bestDist = std::numeric_limits<double>::infinity();
    double bestT = std::numeric_limits<double>::infinity();
    int bestEdge = -1;

    for (size_t i = 0; i < edges.edges.size(); ++i) {
        if (candidate[i] == 0) continue;
        const EdgeRecord &e = edges.edges[i];

        const Vec3d p0 = mesh_pos(mesh, e.v0);
        const Vec3d p1 = mesh_pos(mesh, e.v1);

        double t = 0.0;
        double d = 0.0;
        if (!segment_ray_distance(p0, p1, rayOrig, rayDir, &t, &d)) continue;

        if (d > pickRadius) continue;
        if (d < bestDist - 1e-9 ||
            (std::fabs(d - bestDist) <= 1e-9 && t < bestT)) {
            bestDist = d;
            bestT = t;
            bestEdge = (int)i;
        }
    }

    if (bestEdge >= 0 && outDistance) *outDistance = bestT;
    return bestEdge;
}

}  // namespace vicad
